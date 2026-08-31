/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ErrSchedulerManager — per-model ERR scheduling state.
 * Create: 2026-08-25
 */

package gs

import (
	"encoding/json"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"huawei.com/aigw/pkg/log"
)

// ErrSchedulerConfig holds configuration for the ERR scheduler.
type ErrSchedulerConfig struct {
	SchedulerType      string // "mej", "err", "caterr"
	WarmupSessions     int    // total observations threshold for warmup (default: 100; 0 = ignore)
	WarmupPerCat       int    // per-category threshold, CatERR only (default: 10; 0 = ignore)
	WindowSize         int    // EmpiricalDist sliding window size (0 = unbounded)
	TrainDataPath      string // path to pre-trained distributions JSON
	TfidfVocabPath     string // path to TF-IDF vocab JSON (CatERR)
	TfidfCentroidsPath string // path to K-means centroids JSON (CatERR)
	SessionTimeoutSec  int    // session idle timeout after tool_calls/other, seconds (default: 660)
	TaskIdleTimeoutSec int    // session idle timeout after stop, seconds (default: 60)
}

// ErrSchedulerManager manages ERR scheduling state for a single model.
type ErrSchedulerManager struct {
	mu             sync.RWMutex
	config         ErrSchedulerConfig
	algorithm      ErrAlgorithm
	globalDist     *EmpiricalDist
	catDists       map[string]*EmpiricalDist
	catMu          sync.RWMutex
	classifier     *CatErrClassifier
	sessionTracker *SessionTracker
	completedCount int64
}

// trainDataFile is the JSON format for pre-trained distributions.
type trainDataFile struct {
	Global  map[int]int            `json:"global"`
	CatDist map[string]map[int]int `json:"cat_dist,omitempty"`
}

func catDistKeys(m map[string]map[int]int) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	return keys
}

// NewErrSchedulerManager creates a new ErrSchedulerManager.
// Returns nil if schedulerType is empty or "none".
func NewErrSchedulerManager(cfg ErrSchedulerConfig) *ErrSchedulerManager {
	if cfg.SchedulerType == "" || cfg.SchedulerType == "none" {
		return nil
	}

	m := &ErrSchedulerManager{
		config:         cfg,
		globalDist:     NewEmpiricalDist(cfg.WindowSize),
		catDists:       make(map[string]*EmpiricalDist),
		classifier:     NewCatErrClassifier(),
		sessionTracker: NewSessionTracker(
			time.Duration(cfg.TaskIdleTimeoutSec)*time.Second,
			time.Duration(cfg.SessionTimeoutSec)*time.Second,
		),
	}

	m.loadTrainingData()
	m.initAlgorithm()

	log.Info().Msgf("[ERR] scheduler initialized: type=%s, warmup=%d, window=%d, completed=%d",
		cfg.SchedulerType, cfg.WarmupSessions, cfg.WindowSize,
		atomic.LoadInt64(&m.completedCount))
	return m
}

// loadTrainingData loads pre-trained distributions and TF-IDF data from files.
func (m *ErrSchedulerManager) loadTrainingData() {
	if m.config.TfidfVocabPath != "" {
		if err := m.classifier.LoadVocab(m.config.TfidfVocabPath); err != nil {
			log.Warn().Msgf("[ERR] failed to load TF-IDF vocab from %s: %v", m.config.TfidfVocabPath, err)
		}
	}

	if m.config.TfidfCentroidsPath != "" {
		if err := m.classifier.LoadCentroids(m.config.TfidfCentroidsPath); err != nil {
			log.Warn().Msgf("[ERR] failed to load centroids from %s: %v", m.config.TfidfCentroidsPath, err)
		}
	}

	if m.config.TrainDataPath == "" {
		return
	}

	bytes, err := os.ReadFile(m.config.TrainDataPath)
	if err != nil {
		log.Warn().Msgf("[ERR] failed to read train data from %s: %v", m.config.TrainDataPath, err)
		return
	}

	var data trainDataFile
	if err := json.Unmarshal(bytes, &data); err != nil {
		log.Warn().Msgf("[ERR] failed to parse train data: %v", err)
		return
	}

	if len(data.Global) > 0 {
		m.globalDist.SeedFromCounts(data.Global)
	}

	for cat, counts := range data.CatDist {
		dist := NewEmpiricalDist(m.config.WindowSize)
		dist.SeedFromCounts(counts)
		m.catDists[cat] = dist
		log.Info().Msgf("[ERR] loaded category %s: %d observations", cat, dist.TotalCount())
	}

	totalObserved := m.globalDist.TotalCount()
	atomic.StoreInt64(&m.completedCount, int64(totalObserved))
	log.Info().Msgf("[ERR] seeded global dist with %d observations", totalObserved)

	// Validate cat_dist seed categories against classifier labels.
	if m.config.SchedulerType == "caterr" && len(data.CatDist) > 0 {
		classifierLabels := make(map[string]bool)
		for _, l := range m.classifier.Labels() {
			classifierLabels[l] = true
		}

		matched := 0
		unmatched := []string{}
		for cat := range data.CatDist {
			if classifierLabels[cat] {
				matched++
			} else {
				unmatched = append(unmatched, cat)
			}
		}

		if matched == 0 {
			log.Warn().Msgf("[ERR] cat_dist seed categories %v do not match any classifier label %v; "+
				"discarding all cat_dist seed, entering warmup",
				catDistKeys(data.CatDist), m.classifier.Labels())
			m.catMu.Lock()
			m.catDists = make(map[string]*EmpiricalDist)
			m.catMu.Unlock()
		} else if len(unmatched) > 0 {
			log.Warn().Msgf("[ERR] cat_dist seed has unmatched categories %v (classifier labels %v); "+
				"discarding unmatched", unmatched, m.classifier.Labels())
			m.catMu.Lock()
			for _, cat := range unmatched {
				delete(m.catDists, cat)
			}
			m.catMu.Unlock()
		} else if len(data.CatDist) > len(classifierLabels) {
			log.Info().Msgf("[ERR] cat_dist seed has extra categories beyond classifier labels %v",
				m.classifier.Labels())
		}
	}
}

// initAlgorithm sets up the ErrAlgorithm based on config.
func (m *ErrSchedulerManager) initAlgorithm() {
	switch m.config.SchedulerType {
	case "mej":
		m.algorithm = NewMejAlg()
	case "err":
		m.algorithm = NewErrAlg(m.globalDist)
	case "caterr":
		m.algorithm = NewCatErrAlg(m.classifier, m.catDists, &m.catMu)
	default:
		log.Warn().Msgf("[ERR] unknown scheduler type %s, falling back to mej", m.config.SchedulerType)
		m.algorithm = NewMejAlg()
	}
}

// OnRequest is called when a new HTTP request arrives.
// Returns the iteration count k for this session.
func (m *ErrSchedulerManager) OnRequest(sessionID, model, prompt string) int {
	k := m.sessionTracker.OnRequest(sessionID, model)
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() && sessionID != "" {
		if cat := m.classifier.Classify(prompt); cat != "" {
			m.sessionTracker.SetCategory(sessionID, cat)
		}
	}
	return k
}

// AnonymousPriority returns the priority for an anonymous request (no session_id).
// Uses the median step count as a hypothetical k so the request sits at p50.
func (m *ErrSchedulerManager) AnonymousPriority(prompt string) float64 {
	medianK := m.globalDist.Median()
	if medianK <= 0 {
		medianK = 1
	}
	if m.IsWarmingUp() {
		return -float64(medianK)
	}
	if m.algorithm != nil {
		return m.algorithm.Priority("", medianK, prompt)
	}
	return -float64(medianK)
}

// Priority computes the scheduling priority for a session at iteration k.
// During warmup, degenerates to MEJ.
func (m *ErrSchedulerManager) Priority(sessionID string, k int, prompt string) float64 {
	if m.IsWarmingUp() {
		return -float64(k)
	}
	return m.algorithm.Priority(sessionID, k, prompt)
}

// OnFinish is called when finish_reason=stop is received.
// Records the session's total steps into the distribution and returns the step count.
func (m *ErrSchedulerManager) OnFinish(sessionID string) int {
	steps := m.sessionTracker.OnFinish(sessionID)
	if steps <= 0 {
		return 0
	}

	m.globalDist.AddValue(sessionID, steps)

	if m.config.SchedulerType == "caterr" {
		if cat := m.sessionTracker.GetCategory(sessionID); cat != "" {
			m.catMu.RLock()
			dist, ok := m.catDists[cat]
			m.catMu.RUnlock()
			if !ok {
				dist = NewEmpiricalDist(m.config.WindowSize)
				m.catMu.Lock()
				if existing, ok := m.catDists[cat]; ok {
					dist = existing
				} else {
					m.catDists[cat] = dist
				}
				m.catMu.Unlock()
			}
			dist.AddValue(sessionID, steps)
			log.Debug().Msgf("[ERR] session %s finished, steps=%d, category=%s", sessionID, steps, cat)
		}
	}

	var count int64
	if m.IsWarmingUp() {
		count = atomic.AddInt64(&m.completedCount, 1)
		if !m.IsWarmingUp() {
			log.Info().Msgf("[ERR] warmup complete, activating %s", m.config.SchedulerType)
		}
	} else {
		count = atomic.LoadInt64(&m.completedCount)
	}

	log.Debug().Msgf("[ERR] session %s finished, steps=%d, total_completed=%d",
		sessionID, steps, count)
	return steps
}

// OnCancel is called when the HTTP connection is closed abnormally.
func (m *ErrSchedulerManager) OnCancel(sessionID string) {
	m.sessionTracker.OnCancel(sessionID)
}

// RestartIdleTimer arms the long idle timer after tool_calls/other finish_reason.
func (m *ErrSchedulerManager) RestartIdleTimer(sessionID string) {
	m.sessionTracker.RestartIdleTimer(sessionID)
}

// IsWarmingUp returns true if the scheduler has not seen enough observations.
// For ERR/MEJ: warmup done when globalDist >= WarmupSessions.
// For CatERR: warmup done when every classifier category has >= WarmupPerCat in catDists.
func (m *ErrSchedulerManager) IsWarmingUp() bool {
	if m.config.SchedulerType == "caterr" {
		if m.config.WarmupPerCat == 0 {
			return false
		}
		labels := m.classifier.Labels()
		m.catMu.RLock()
		defer m.catMu.RUnlock()
		if len(labels) == 0 {
			return true
		}
		for _, label := range labels {
			dist, ok := m.catDists[label]
			if !ok || dist.TotalCount() < m.config.WarmupPerCat {
				return true
			}
		}
		return false
	}

	if m.config.WarmupSessions > 0 && m.globalDist.TotalCount() < m.config.WarmupSessions {
		return true
	}
	return false
}

// AlgorithmName returns the name of the active algorithm.
func (m *ErrSchedulerManager) AlgorithmName() string {
	if m.IsWarmingUp() {
		return "mej (warmup)"
	}
	if m.algorithm != nil {
		return m.algorithm.Name()
	}
	return "none"
}

// CompletedCount returns the number of completed sessions.
func (m *ErrSchedulerManager) CompletedCount() int64 {
	return atomic.LoadInt64(&m.completedCount)
}
