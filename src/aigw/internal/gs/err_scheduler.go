/*
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
	WarmupSessions     int    // total observations threshold for warmup completion (default: 100; 0 = ignore total)
	WarmupPerCat       int    // per-category observations threshold for warmup completion (CatERR only, default: 10; 0 = ignore per-category)
	MaxK               int    // MEJ offset / E[rem|k] max
	WindowSize         int    // EmpiricalDist sliding window size (0 = unbounded)
	TrainDataPath      string // path to pre-trained per-category distributions JSON
	TfidfVocabPath     string // path to TF-IDF vocab JSON (for vectorizer.LoadVectorizer)
	TfidfCentroidsPath string // path to K-means centroids JSON
	SessionTimeout     int    // idle-cleanup timeout after tool_calls/other (review [20], §5.1); 0 -> default 660
	TaskIdleTimeout    int    // idle-cleanup timeout after stop (§5.1: stop+60s = normal completion); 0 -> default 60
}

// ErrSchedulerManager manages the ERR scheduling state for a single model.
// It owns the algorithm, distribution(s), session tracker, and warmup counter.
type ErrSchedulerManager struct {
	mu sync.RWMutex

	config    ErrSchedulerConfig
	algorithm ErrAlgorithm

	// ERR distribution (global, per-model)
	globalDist *EmpiricalDist

	// CatERR: per-category distributions
	catDists map[string]*EmpiricalDist

	// catMu protects the catDists map against runtime writes (OnFinish creates
	// new category entries on first observation). Shared with CatErrAlg.
	catMu sync.RWMutex

	// TF-IDF classifier for CatERR
	classifier *CatErrClassifier

	// Session tracking
	sessionTracker *SessionTracker

	// Warmup counter
	completedCount int64
}

// trainDataFile is the JSON format for pre-trained distributions.
type trainDataFile struct {
	Global  map[int]int            `json:"global"`             // {steps: count}
	CatDist map[string]map[int]int `json:"cat_dist,omitempty"` // {category: {steps: count}}
}

// NewErrSchedulerManager creates a new ErrSchedulerManager.
// Returns nil if schedulerType is empty or "none".
func NewErrSchedulerManager(cfg ErrSchedulerConfig) *ErrSchedulerManager {
	if cfg.SchedulerType == "" || cfg.SchedulerType == "none" {
		return nil
	}

	// WarmupSessions/WarmupPerCat: negative → 0. If BOTH are 0 (neither
	// configured), apply defaults (100/10) — covers the "field omitted entirely"
	// case. If at least one is explicitly set (even to 0), respect it: 0 means
	// "ignore that threshold". E.g. WarmupSessions=0 + WarmupPerCat=10 → only
	// per-category gate; WarmupSessions=100 + WarmupPerCat=0 → only total gate.
	if cfg.WarmupSessions < 0 {
		cfg.WarmupSessions = 0
	}
	if cfg.WarmupPerCat < 0 {
		cfg.WarmupPerCat = 0
	}
	if cfg.WarmupSessions == 0 && cfg.WarmupPerCat == 0 {
		cfg.WarmupSessions = 100
		cfg.WarmupPerCat = 10
	}
	if cfg.MaxK <= 0 {
		cfg.MaxK = 10000
	}
	if cfg.SessionTimeout <= 0 {
		cfg.SessionTimeout = 660 // default post-tool_calls idle cleanup (review [20], §5.1)
	}
	if cfg.TaskIdleTimeout <= 0 {
		cfg.TaskIdleTimeout = 60 // default post-stop idle cleanup (§5.1: stop+60s = normal completion)
	}

	m := &ErrSchedulerManager{
		config:         cfg,
		globalDist:     NewEmpiricalDist(cfg.WindowSize),
		catDists:       make(map[string]*EmpiricalDist),
		classifier:     NewCatErrClassifier(),
		sessionTracker: NewSessionTracker(
			time.Duration(cfg.TaskIdleTimeout)*time.Second,
			time.Duration(cfg.SessionTimeout)*time.Second,
		),
	}

	// Load training data if available
	m.loadTrainingData()

	// Set up algorithm
	m.initAlgorithm()

	log.Info().Msgf("[ERR] scheduler initialized: type=%s, warmup=%d, maxK=%d, window=%d, completed=%d",
		cfg.SchedulerType, cfg.WarmupSessions, cfg.MaxK, cfg.WindowSize,
		atomic.LoadInt64(&m.completedCount))
	return m
}

// loadTrainingData loads pre-trained distributions and TF-IDF data from files.
func (m *ErrSchedulerManager) loadTrainingData() {
	// Load TF-IDF vocab (self-contained, does not use global vectorizer package)
	if m.config.TfidfVocabPath != "" {
		if err := m.classifier.LoadVocab(m.config.TfidfVocabPath); err != nil {
			log.Warn().Msgf("[ERR] failed to load TF-IDF vocab from %s: %v", m.config.TfidfVocabPath, err)
		}
	}

	// Load centroids
	if m.config.TfidfCentroidsPath != "" {
		if err := m.classifier.LoadCentroids(m.config.TfidfCentroidsPath); err != nil {
			log.Warn().Msgf("[ERR] failed to load centroids from %s: %v", m.config.TfidfCentroidsPath, err)
		}
	}

	// Load distribution data
	if m.config.TrainDataPath != "" {
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

		// Seeded data counts toward warmup
		totalObserved := m.globalDist.TotalCount()
		atomic.StoreInt64(&m.completedCount, int64(totalObserved))
		log.Info().Msgf("[ERR] seeded global dist with %d observations", totalObserved)
	}
	// Note: an empty trainDataPath is fine for ERR/MEJ (they warm up online);
	// CatERR's required TF-IDF data is validated in initErrScheduler (blocks
	// startup if missing). See review [7].
}

// initAlgorithm sets up the ErrAlgorithm based on config.
func (m *ErrSchedulerManager) initAlgorithm() {
	switch m.config.SchedulerType {
	case "mej":
		m.algorithm = NewMejAlg(m.config.MaxK)
	case "err":
		m.algorithm = NewErrAlg(m.globalDist)
	case "caterr":
		m.algorithm = NewCatErrAlg(m.classifier, m.catDists, m.globalDist, &m.catMu)
	default:
		log.Warn().Msgf("[ERR] unknown scheduler type %s, falling back to mej", m.config.SchedulerType)
		m.algorithm = NewMejAlg(m.config.MaxK)
	}
}

// OnRequest is called when a new HTTP request arrives.
// Returns the iteration count k for this session.
func (m *ErrSchedulerManager) OnRequest(sessionID, model, prompt string) int {
	k := m.sessionTracker.OnRequest(sessionID, model)
	// E3: classify the prompt now and store the category on the session, so
	// OnFinish can update the per-category distribution. See review [29].
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() && sessionID != "" {
		if cat := m.classifier.Classify(prompt); cat != "" {
			m.sessionTracker.SetCategory(sessionID, cat)
		}
	}
	return k
}

// AnonymousPriority returns the priority for an anonymous request (no session_id).
// Uses the median step count as a hypothetical k so the request sits at p50 of
// the priority spectrum instead of always last (k=1 → E[rem|1] is the max).
// See review E1.
func (m *ErrSchedulerManager) AnonymousPriority(prompt string) float64 {
	medianK := m.globalDist.Median()
	if medianK <= 0 {
		medianK = 1
	}
	if m.IsWarmingUp() {
		return float64(m.config.MaxK + 1 - medianK)
	}
	if m.algorithm != nil {
		return m.algorithm.Priority("", medianK, prompt)
	}
	return float64(m.config.MaxK + 1 - medianK)
}

// Priority computes the scheduling priority for a session at iteration k.
// During warmup, degenerates to MEJ.
func (m *ErrSchedulerManager) Priority(sessionID string, k int, prompt string) float64 {
	if m.IsWarmingUp() {
		return float64(m.config.MaxK + 1 - k)
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

	// Add to distribution (per-session single-record: a session with multiple
	// stops updates its single entry to the latest step count, see [21]/坑-1).
	m.globalDist.AddValue(sessionID, steps)

	// E3: also add to the per-category distribution (CatERR). The category was
	// classified at OnRequest and stored on the session, so we don't need the
	// prompt here. See review [29].
	if m.config.SchedulerType == "caterr" {
		if cat := m.sessionTracker.GetCategory(sessionID); cat != "" {
			m.catMu.RLock()
			dist, ok := m.catDists[cat]
			m.catMu.RUnlock()
			if !ok {
				// First observation for this category: create the dist.
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

	// completedCount is a log-only metric (IsWarmingUp now queries
	// globalDist/catDists TotalCount directly, see [19]). Keep incrementing it
	// during warmup for the "warmup complete" log line; it's no longer the
	// warmup gate. After warmup it freezes (avoids unbounded growth / overflow).
	var count int64
	if m.IsWarmingUp() {
		count = atomic.AddInt64(&m.completedCount, 1)
		// Log-only: note that for CatERR the actual gate is the dual-threshold
		// in IsWarmingUp (total + per-category); this line just tracks progress.
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

// RestartIdleTimer arms the long (sessionTimeout, §5.1: 660s) idle timer after
// a tool_calls/other finish_reason. tool_calls+660s no new request and no TCP
// close = agent crash/disconnect (extreme anomaly) → session cleaned up (step
// count NOT recorded). See review [20].
func (m *ErrSchedulerManager) RestartIdleTimer(sessionID string) {
	m.sessionTracker.RestartIdleTimer(sessionID)
}

// IsWarmingUp returns true if the scheduler hasn't seen enough observations.
//
// For ERR/MEJ: warmup is done when globalDist has >= WarmupSessions observations.
//
// For CatERR: warmup is done when BOTH thresholds are met:
//   - globalDist total >= WarmupSessions (0 = skip this check)
//   - every known category's catDists[cat] total >= WarmupPerCat (0 = skip)
//
// During warmup, Priority returns MEJ (not E[rem]) for ALL categories — so
// categories never mix E[rem] (some warm, some not) vs MEJ (-k, always lower),
// which would starve the MEJ ones. Only after ALL categories are warm do we
// switch to CatERR (all E[rem]). See review [19].
func (m *ErrSchedulerManager) IsWarmingUp() bool {
	// Global total threshold (applies to all algorithm types).
	if m.config.WarmupSessions > 0 && m.globalDist.TotalCount() < m.config.WarmupSessions {
		return true
	}

	// Per-category threshold (CatERR only).
	if m.config.SchedulerType == "caterr" && m.config.WarmupPerCat > 0 {
		m.catMu.RLock()
		for _, dist := range m.catDists {
			if dist.TotalCount() < m.config.WarmupPerCat {
				m.catMu.RUnlock()
				return true // at least one category not warm enough
			}
		}
		m.catMu.RUnlock()
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
