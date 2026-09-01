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
	WarmupSessions     int
	WarmupPerCat       int
	WindowSize         int
	TrainDataPath      string
	TfidfVocabPath     string
	TfidfCentroidsPath string
	SessionTimeoutSec  int
	TaskIdleTimeoutSec int
}

// algoSnapshot holds immutable dist snapshots for one Pick iteration.
// Guarantees all E[rem|k] queries within a single Pick use the same table.
type algoSnapshot struct {
	global *distSnapshot              // for ERR; nil for CatERR
	cats   map[string]*distSnapshot   // for CatERR; nil for ERR
}

type ErrSchedulerManager struct {
	config         ErrSchedulerConfig
	algorithm      ErrAlgorithm
	globalDist     *EmpiricalDist // nil for CatERR
	catDists       map[string]*EmpiricalDist
	catMu          sync.RWMutex
	classifier     *CatErrClassifier
	sessionTracker *SessionTracker
	completedCount int64
}

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

func NewErrSchedulerManager(cfg ErrSchedulerConfig) *ErrSchedulerManager {
	if cfg.SchedulerType == "" || cfg.SchedulerType == "none" {
		return nil
	}

	isCaterr := cfg.SchedulerType == "caterr"

	m := &ErrSchedulerManager{
		config:         cfg,
		classifier:     NewCatErrClassifier(),
		sessionTracker: NewSessionTracker(
			time.Duration(cfg.TaskIdleTimeoutSec)*time.Second,
			time.Duration(cfg.SessionTimeoutSec)*time.Second,
		),
	}
	if !isCaterr {
		m.globalDist = NewEmpiricalDist(cfg.WindowSize)
	} else {
		m.catDists = make(map[string]*EmpiricalDist)
	}

	m.loadTrainingData()
	m.initAlgorithm()

	log.Info().Msgf("[ERR] scheduler initialized: type=%s, warmup=%d, window=%d, completed=%d",
		cfg.SchedulerType, cfg.WarmupSessions, cfg.WindowSize,
		atomic.LoadInt64(&m.completedCount))
	return m
}

func (m *ErrSchedulerManager) loadTrainingData() {
	if m.config.TfidfVocabPath != "" {
		m.classifier.LoadVocab(m.config.TfidfVocabPath)
	}
	if m.config.TfidfCentroidsPath != "" {
		m.classifier.LoadCentroids(m.config.TfidfCentroidsPath)
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

	if m.globalDist != nil && len(data.Global) > 0 {
		m.globalDist.SeedFromCounts(data.Global)
	}

	if m.catDists != nil {
		for cat, counts := range data.CatDist {
			dist := NewEmpiricalDist(m.config.WindowSize)
			dist.SeedFromCounts(counts)
			m.catDists[cat] = dist
		}
	}

	totalObserved := 0
	if m.globalDist != nil {
		totalObserved = m.globalDist.TotalCount()
	} else if m.catDists != nil {
		m.catMu.RLock()
		for _, d := range m.catDists {
			totalObserved += d.TotalCount()
		}
		m.catMu.RUnlock()
	}
	atomic.StoreInt64(&m.completedCount, int64(totalObserved))
	log.Info().Msgf("[ERR] seeded %d observations", totalObserved)

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
		}
	}
}

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

func (m *ErrSchedulerManager) OnRequest(sessionID, model, prompt string) int {
	k := m.sessionTracker.OnRequest(sessionID, model)
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() && sessionID != "" {
		if cat := m.classifier.Classify(prompt); cat != "" {
			m.sessionTracker.SetCategory(sessionID, cat)
		}
	}
	return k
}

func (m *ErrSchedulerManager) AnonymousPriority(prompt string) float64 {
	medianK := m.medianKForAnonymous(prompt)
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

// AnonymousPriorityWith uses the provided snapshot for consistent reads within one Pick.
func (m *ErrSchedulerManager) AnonymousPriorityWith(snap *algoSnapshot, prompt string) float64 {
	medianK := m.medianKForAnonymousWith(snap, prompt)
	if medianK <= 0 {
		medianK = 1
	}
	if m.IsWarmingUp() {
		return -float64(medianK)
	}
	if m.algorithm != nil {
		return m.algorithm.PriorityWith(snap, "", medianK, prompt)
	}
	return -float64(medianK)
}

func (m *ErrSchedulerManager) medianKForAnonymous(prompt string) int {
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() {
		if cat := m.classifier.Classify(prompt); cat != "" {
			m.catMu.RLock()
			dist, ok := m.catDists[cat]
			m.catMu.RUnlock()
			if ok {
				return dist.Median()
			}
		}
	}
	if m.globalDist != nil {
		return m.globalDist.Median()
	}
	return 0
}

func (m *ErrSchedulerManager) medianKForAnonymousWith(snap *algoSnapshot, prompt string) int {
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() {
		if cat := m.classifier.Classify(prompt); cat != "" {
			if snap.cats != nil {
				if s, ok := snap.cats[cat]; ok {
					return s.Median()
				}
			}
		}
	}
	if snap.global != nil {
		return snap.global.Median()
	}
	return 0
}

func (m *ErrSchedulerManager) Priority(sessionID string, k int, prompt string) float64 {
	if m.IsWarmingUp() {
		return -float64(k)
	}
	return m.algorithm.Priority(sessionID, k, prompt)
}

// PriorityWith uses the provided snapshot for consistent reads within one Pick.
func (m *ErrSchedulerManager) PriorityWith(snap *algoSnapshot, sessionID string, k int, prompt string) float64 {
	if m.IsWarmingUp() {
		return -float64(k)
	}
	return m.algorithm.PriorityWith(snap, sessionID, k, prompt)
}

// AlgoSnapshot loads all dist snapshots for consistent reads within one Pick.
func (m *ErrSchedulerManager) AlgoSnapshot() *algoSnapshot {
	s := &algoSnapshot{}
	if m.globalDist != nil {
		s.global = m.globalDist.Snapshot()
	}
	if m.catDists != nil {
		m.catMu.RLock()
		s.cats = make(map[string]*distSnapshot, len(m.catDists))
		for cat, dist := range m.catDists {
			s.cats[cat] = dist.Snapshot()
		}
		m.catMu.RUnlock()
	}
	return s
}

func (m *ErrSchedulerManager) OnFinish(sessionID string) int {
	steps := m.sessionTracker.OnFinish(sessionID)
	if steps <= 0 {
		return 0
	}

	if m.globalDist != nil {
		m.globalDist.AddValue(sessionID, steps)
		m.globalDist.Refresh()
	}

	if m.config.SchedulerType == "caterr" {
		if cat := m.sessionTracker.GetCategory(sessionID); cat != "" {
			m.catMu.Lock()
			dist, ok := m.catDists[cat]
			if !ok {
				dist = NewEmpiricalDist(m.config.WindowSize)
				m.catDists[cat] = dist
			}
			m.catMu.Unlock()
			dist.AddValue(sessionID, steps)
			dist.Refresh()
		}
	}

	if m.IsWarmingUp() {
		atomic.AddInt64(&m.completedCount, 1)
		if !m.IsWarmingUp() {
			log.Info().Msgf("[ERR] warmup complete, activating %s", m.config.SchedulerType)
		}
	}

	return steps
}

func (m *ErrSchedulerManager) OnCancel(sessionID string) {
	m.sessionTracker.OnCancel(sessionID)
}

func (m *ErrSchedulerManager) RestartIdleTimer(sessionID string) {
	m.sessionTracker.RestartIdleTimer(sessionID)
}

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

func (m *ErrSchedulerManager) AlgorithmName() string {
	if m.IsWarmingUp() {
		return "mej (warmup)"
	}
	if m.algorithm != nil {
		return m.algorithm.Name()
	}
	return "none"
}

func (m *ErrSchedulerManager) CompletedCount() int64 {
	return atomic.LoadInt64(&m.completedCount)
}

func (m *ErrSchedulerManager) SessionCategory(sessionID string) string {
	return m.sessionTracker.GetCategory(sessionID)
}

func (m *ErrSchedulerManager) SessionIteration(sessionID string) int {
	return m.sessionTracker.GetIteration(sessionID)
}
