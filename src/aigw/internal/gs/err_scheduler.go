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
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"huawei.com/aigw/pkg/log"
)

// ErrSchedulerConfig holds configuration for the ERR scheduler.
type ErrSchedulerConfig struct {
	SchedulerType      string // "mej", "err", "caterr", "fifo"
	WarmupSessions     int
	WarmupPerCat       int
	WindowSize         int
	TrainDataPath      string
	TfidfVocabPath     string
	TfidfCentroidsPath string
	SessionTimeoutSec  int
	TaskIdleTimeoutSec int
	QueueBoostSec      int
}

// algoSnapshot holds immutable dist snapshots for one Pick iteration.
// Guarantees all E[rem|k] queries within a single Pick use the same table.
type algoSnapshot struct {
	global *distSnapshot            // for ERR; nil for CatERR
	cats   map[string]*distSnapshot // for CatERR; nil for ERR
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

	// warmupAnnounced makes the completion log exactly-once; guarded by the
	// dispatch mutex all OnFinish callers hold.
	warmupAnnounced bool
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

// defaultQueueBoostSec is the starvation floor: a request queued longer than
// this is forced to top (0) priority so sustained overload cannot starve it
// into the client's first-chunk budget. Negative disables the floor.
const defaultQueueBoostSec = 240

func NewErrSchedulerManager(cfg ErrSchedulerConfig) *ErrSchedulerManager {
	if cfg.SchedulerType == "" || cfg.SchedulerType == "none" {
		return nil
	}
	if cfg.QueueBoostSec == 0 {
		cfg.QueueBoostSec = defaultQueueBoostSec
	}

	isCaterr := cfg.SchedulerType == "caterr"

	m := &ErrSchedulerManager{
		config:     cfg,
		classifier: NewCatErrClassifier(),
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
		alg := NewCatErrAlg(m.classifier, m.catDists, &m.catMu)
		alg.CategoryFor = m.sessionTracker.GetCategory
		m.algorithm = alg
	case "fifo":
		m.algorithm = NewFifoAlg()
	default:
		log.Warn().Msgf("[ERR] unknown scheduler type %s, falling back to mej", m.config.SchedulerType)
		m.algorithm = NewMejAlg()
	}
}

func (m *ErrSchedulerManager) OnRequest(sessionID, model, prompt string) int {
	curStep := 1
	if parent := parentSessionID(sessionID); parent != "" {
		// Sub-agents run synchronously inside the parent's tool call: the
		// parent is blocked waiting, so the sub-agent's progress IS the
		// parent's progress. Starting at k=1 would rank it as a fresh
		// longest-remaining task (SJF's worst tier) exactly when the
		// parent is closest to completion; inherit the parent's k so
		// both rank identically.
		curStep = m.sessionTracker.OnRequestWithBase(sessionID, model,
			m.sessionTracker.GetIteration(parent))
	} else {
		curStep = m.sessionTracker.OnRequest(sessionID, model)
	}
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() && sessionID != "" {
		m.lockSessionCategory(sessionID, prompt)
	}
	return curStep
}

// lockSessionCategory assigns the session's category exactly once.
// Sub-agent sessions ("{parent}_sub_...") inherit the parent's category;
// unclassifiable requests lock nothing.
func (m *ErrSchedulerManager) lockSessionCategory(sessionID, prompt string) {
	if m.sessionTracker.GetCategory(sessionID) != "" {
		return
	}
	if parent := parentSessionID(sessionID); parent != "" {
		if cat := m.sessionTracker.GetCategory(parent); cat != "" {
			m.sessionTracker.SetCategory(sessionID, cat)
			return
		}
	}
	if cat := m.classifier.Classify(prompt); cat != "" {
		m.sessionTracker.SetCategory(sessionID, cat)
	}
}

// parentSessionID returns the parent of an SDK sub-agent session id
// ("{parent}_sub_{type}_{uuid}"), or "" when the id carries no sub marker.
func parentSessionID(sessionID string) string {
	if idx := strings.Index(sessionID, "_sub_"); idx > 0 {
		return sessionID[:idx]
	}
	return ""
}

func (m *ErrSchedulerManager) AnonymousPriority(prompt string) float64 {
	if m.algorithm != nil && m.algorithm.Name() == "fifo" {
		return 0
	}
	medianStep := m.medianStepForAnonymous(prompt)
	if medianStep <= 0 {
		medianStep = 1
	}
	if m.IsWarmingUp() {
		return -float64(medianStep)
	}
	if m.algorithm != nil {
		return m.algorithm.Priority("", medianStep, prompt)
	}
	return -float64(medianStep)
}

// AnonymousPriorityWith uses the provided snapshot for consistent reads within one Pick.

func (m *ErrSchedulerManager) medianStepForAnonymous(prompt string) int {
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() {
		if cat := m.classifier.Classify(prompt); cat != "" {
			m.catMu.RLock()
			dist, ok := m.catDists[cat]
			m.catMu.RUnlock()
			if ok {
				return dist.Median()
			}
		}
		m.catMu.RLock()
		med, ok := pooledMedianStep(m.catDists)
		m.catMu.RUnlock()
		if ok {
			return med
		}
	}
	if m.globalDist != nil {
		return m.globalDist.Median()
	}
	return 0
}

func (m *ErrSchedulerManager) medianStepForAnonymousWith(snap *algoSnapshot, prompt string) int {
	if m.config.SchedulerType == "caterr" && m.classifier.IsLoaded() {
		if cat := m.classifier.Classify(prompt); cat != "" {
			if snap.cats != nil {
				if s, ok := snap.cats[cat]; ok {
					return s.Median()
				}
			}
		}
		// Unclassifiable (zero vocabulary hit): synthesize the step from the
		// pooled categories so the request ranks as a typical task instead
		// of bottoming out at step 1.
		if med, ok := pooledMedianStep(snap.cats); ok {
			return med
		}
	}
	if snap.global != nil {
		return snap.global.Median()
	}
	return 0
}

// pooledMedianStep: count-weighted mean of per-category medians.
func pooledMedianStep[D interface {
	TotalCount() int
	Median() int
}](cats map[string]D) (int, bool) {
	var weighted float64
	var total int64
	for _, s := range cats {
		n := int64(s.TotalCount())
		if n <= 0 {
			continue
		}
		weighted += float64(s.Median()) * float64(n)
		total += n
	}
	if total == 0 {
		return 0, false
	}
	return int(weighted / float64(total)), true
}

func (m *ErrSchedulerManager) Priority(sessionID string, curStep int, prompt string) float64 {
	if m.IsWarmingUp() {
		return -float64(curStep)
	}
	return m.algorithm.Priority(sessionID, curStep, prompt)
}

// PriorityWith uses the provided snapshot for consistent reads within one Pick.
func (m *ErrSchedulerManager) PriorityWith(snap *algoSnapshot, sessionID string, curStep int, prompt string) float64 {
	if m.IsWarmingUp() {
		return -float64(curStep)
	}
	return m.algorithm.PriorityWith(snap, sessionID, curStep, prompt)
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
		m.globalDist.MaybeRefresh()
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
			dist.MaybeRefresh()
		}
	}

	if m.IsWarmingUp() {
		atomic.AddInt64(&m.completedCount, 1)
		if !m.warmupAnnounced && m.warmupCompleteLive() {
			m.warmupAnnounced = true
			log.Info().Msgf("[ERR] warmup complete, activating %s", m.config.SchedulerType)
		}
	}

	log.Debug().Msgf("[ERR] session=%s finished: steps=%d cat=%s", sessionID, steps, m.sessionTracker.GetCategory(sessionID))
	return steps
}

// OnQueueTimeout handles a queued request that hit the client's wait budget
// before dispatch: roll back its iteration so the retry re-enters at the
// same step instead of the session restarting at k=1.
func (m *ErrSchedulerManager) OnQueueTimeout(sessionID string, waited time.Duration) {
	m.sessionTracker.RollbackRequest(sessionID, waited)
}

func (m *ErrSchedulerManager) RollbackInFlightAttempt(sessionID string, queueWait time.Duration) {
	m.sessionTracker.RollbackRequest(sessionID, queueWait)
}

func (m *ErrSchedulerManager) TakeQueueWaitCarry(sessionID string) time.Duration {
	return m.sessionTracker.TakeQueueWaitCarry(sessionID)
}

func (m *ErrSchedulerManager) OnCancel(sessionID string) {
	m.sessionTracker.OnCancel(sessionID)
}

func (m *ErrSchedulerManager) RestartIdleTimer(sessionID string) {
	m.sessionTracker.RestartIdleTimer(sessionID)
}

// warmupCompleteLive mirrors IsWarmingUp against live counters instead of
// snapshots: the completion log must not depend on the 1s snapshot refresh
// cooldown, or the threshold-crossing completion can go unannounced.
func (m *ErrSchedulerManager) warmupCompleteLive() bool {
	if m.config.SchedulerType == "caterr" {
		labels := m.classifier.Labels()
		m.catMu.RLock()
		defer m.catMu.RUnlock()
		if m.config.WarmupSessions > 0 {
			total := 0
			for _, dist := range m.catDists {
				total += dist.LiveCount()
			}
			if total < m.config.WarmupSessions {
				return false
			}
		}
		if m.config.WarmupPerCat > 0 {
			if len(labels) == 0 {
				return false
			}
			for _, label := range labels {
				dist, ok := m.catDists[label]
				if !ok || dist.LiveCount() < m.config.WarmupPerCat {
					return false
				}
			}
		}
		return true
	}
	if m.config.WarmupSessions > 0 && m.globalDist.LiveCount() < m.config.WarmupSessions {
		return false
	}
	return true
}

func (m *ErrSchedulerManager) IsWarmingUp() bool {
	if m.config.SchedulerType == "caterr" {
		labels := m.classifier.Labels()
		m.catMu.RLock()
		defer m.catMu.RUnlock()
		// Warmup is all-or-nothing: every enabled gate must pass before the
		// algorithm ranks with distributions. The total gate (sum across
		// categories) and the per-category gate are independent dimensions;
		// a gate set to 0 opts out of that dimension.
		if m.config.WarmupSessions > 0 {
			total := 0
			for _, dist := range m.catDists {
				total += dist.TotalCount()
			}
			if total < m.config.WarmupSessions {
				return true
			}
		}
		if m.config.WarmupPerCat > 0 {
			if len(labels) == 0 {
				return true
			}
			for _, label := range labels {
				dist, ok := m.catDists[label]
				if !ok || dist.TotalCount() < m.config.WarmupPerCat {
					return true
				}
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
