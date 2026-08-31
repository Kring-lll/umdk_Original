/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: EmpiricalDist for ERR scheduler — E[rem|k] computation.
 * Create: 2026-08-25
 */

package gs

import (
	"container/list"
	"sort"
	"sync"

	"huawei.com/aigw/pkg/log"
)

// EmpiricalDist maintains an empirical distribution of session step counts
// and supports O(log n) E[rem|k] queries via prefix-sum + binary search.
//
// E[rem|k] = E[L - k | L >= k] = (Σ_{L>=k} (L-k)·count(L)) / (Σ_{L>=k} count(L))
//
// When k exceeds the maximum observed step count, ExpectedRemaining returns 0.0
// (MEJ semantics: highest k = most likely near completion = highest priority).
//
// Per-session single-record semantics (review [21]/坑-1): one session_id
// contributes at most ONE step-count entry — its latest. A subsequent stop for
// the same session REPLACES the previous entry (remove-old + add-new) instead
// of appending a duplicate, so a session with stops at k=10 then k=20 leaves
// only the 20 in the distribution. Seeded (anonymous) entries from SeedFromCounts
// are immutable observations (sessionID="") and are never replaced.
//
// The distribution supports a FIFO sliding window (windowSize > 0) to bound
// memory usage, keeping only the most recent windowSize entries.
// windowSize == 0 means unbounded.
type distEntry struct {
	sessionID string // "" for seeded/anonymous (immutable)
	steps     int
}

type EmpiricalDist struct {
	mu            sync.RWMutex
	windowSize    int
	rawValues     *list.List                 // FIFO deque of distEntry
	sessionValues map[string]*list.Element   // sessionID → its current entry (for O(1) replace); "" sessions not tracked
	dirty         bool

	// cached state (rebuilt lazily when dirty)
	sortedVals []int   // sorted copy of rawValues steps
	prefixSum  []int64 // prefix[i] = sum of sortedVals[0..i-1]
	erCache    map[int]float64
	totalCount int
}

// NewEmpiricalDist creates a new EmpiricalDist with optional sliding window.
// windowSize > 0 enables FIFO sliding window; windowSize == 0 means unbounded.
func NewEmpiricalDist(windowSize int) *EmpiricalDist {
	d := &EmpiricalDist{
		windowSize:    windowSize,
		rawValues:     list.New(),
		sessionValues: make(map[string]*list.Element),
		erCache:       make(map[int]float64),
		dirty:         true,
	}
	return d
}

// evictFront removes the front (oldest) entry and, if it belonged to a tracked
// session, drops that session from sessionValues (its current value is gone).
// Caller must hold d.mu.
func (d *EmpiricalDist) evictFront() {
	front := d.rawValues.Front()
	if front == nil {
		return
	}
	if entry, ok := front.Value.(distEntry); ok && entry.sessionID != "" {
		// Only delete if the map still points at THIS element (it hasn't been
		// superseded by a newer entry for the same session).
		if d.sessionValues[entry.sessionID] == front {
			delete(d.sessionValues, entry.sessionID)
		}
	}
	d.rawValues.Remove(front)
}

// AddValue records (or updates) a session's step count. If the session already
// has an entry, the old entry is replaced (so one session_id → at most one
// record, its latest step count). See review [21]/坑-1.
func (d *EmpiricalDist) AddValue(sessionID string, steps int) {
	if steps <= 0 {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()

	// Replace the session's previous entry (no-op if it was already evicted by
	// the sliding window — list.Remove on a non-member returns nil).
	if oldElem, ok := d.sessionValues[sessionID]; ok && oldElem != nil {
		d.rawValues.Remove(oldElem)
	}

	dirty := d.windowSize > 0 && d.rawValues.Len() >= d.windowSize
	_ = dirty
	d.rawValues.PushBack(distEntry{sessionID: sessionID, steps: steps})
	if d.windowSize > 0 && d.rawValues.Len() > d.windowSize {
		d.evictFront()
	}
	if sessionID != "" {
		d.sessionValues[sessionID] = d.rawValues.Back()
	}
	d.dirty = true
}

// SeedFromCounts bulk-loads from a {steps: count} map (e.g. from training data).
// Seeded entries are anonymous (sessionID=""): immutable, never replaced by a
// runtime session update.
func (d *EmpiricalDist) SeedFromCounts(counts map[int]int) {
	if len(counts) == 0 {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	for steps, cnt := range counts {
		if steps <= 0 || cnt <= 0 {
			continue
		}
		for i := 0; i < cnt; i++ {
			d.rawValues.PushBack(distEntry{sessionID: "", steps: steps})
			if d.windowSize > 0 && d.rawValues.Len() > d.windowSize {
				d.evictFront()
			}
		}
	}
	d.dirty = true
}

// ExpectedRemaining returns E[rem|k] = E[L-k | L>=k].
// Returns 0.0 when k is beyond the distribution's support (no L >= k exists).
func (d *EmpiricalDist) ExpectedRemaining(k int) float64 {
	d.mu.RLock()
	if d.dirty {
		d.mu.RUnlock()
		d.rebuild()
		d.mu.RLock()
	}
	defer d.mu.RUnlock()

	if len(d.sortedVals) == 0 {
		return 0.0
	}
	return d.erCache[k]
}

// TotalCount returns the number of observed sessions.
func (d *EmpiricalDist) TotalCount() int {
	d.mu.RLock()
	if d.dirty {
		d.mu.RUnlock()
		d.rebuild()
		d.mu.RLock()
	}
	defer d.mu.RUnlock()
	return d.totalCount
}

// MaxStep returns the maximum observed step count (0 if empty).
func (d *EmpiricalDist) MaxStep() int {
	d.mu.RLock()
	if d.dirty {
		d.mu.RUnlock()
		d.rebuild()
		d.mu.RLock()
	}
	defer d.mu.RUnlock()
	if len(d.sortedVals) == 0 {
		return 0
	}
	return d.sortedVals[len(d.sortedVals)-1]
}

// Median returns the 50th-percentile step count (0 if empty). Used to place
// anonymous requests at p50 of the priority spectrum: an anonymous request is
// treated as if it has already completed the median number of rounds, so its
// E[rem|median] sits in the middle rather than always last (k=1) or first.
func (d *EmpiricalDist) Median() int {
	d.mu.RLock()
	if d.dirty {
		d.mu.RUnlock()
		d.rebuild()
		d.mu.RLock()
	}
	defer d.mu.RUnlock()
	if len(d.sortedVals) == 0 {
		return 0
	}
	return d.sortedVals[len(d.sortedVals)/2]
}

// rebuild recomputes sortedVals, prefixSum, and erCache from rawValues.
// Must be called without any lock held (acquires write lock internally).
func (d *EmpiricalDist) rebuild() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !d.dirty {
		return
	}

	n := d.rawValues.Len()
	if n == 0 {
		d.sortedVals = nil
		d.prefixSum = nil
		d.erCache = make(map[int]float64)
		d.totalCount = 0
		d.dirty = false
		return
	}

	d.sortedVals = make([]int, 0, n)
	for e := d.rawValues.Front(); e != nil; e = e.Next() {
		d.sortedVals = append(d.sortedVals, e.Value.(distEntry).steps)
	}
	sort.Ints(d.sortedVals)
	d.totalCount = n

	// prefix[i] = sum of sortedVals[0..i-1], prefix[0] = 0
	d.prefixSum = make([]int64, n+1)
	for i := 0; i < n; i++ {
		d.prefixSum[i+1] = d.prefixSum[i] + int64(d.sortedVals[i])
	}

	// Precompute E[rem|k] for k = 1..maxStep using binary search
	d.erCache = make(map[int]float64, d.sortedVals[n-1])
	maxStep := d.sortedVals[n-1]
	for k := 1; k <= maxStep; k++ {
		idx := sort.SearchInts(d.sortedVals, k)
		nRem := n - idx
		if nRem == 0 {
			d.erCache[k] = 0.0
			continue
		}
		suffixSum := d.prefixSum[n] - d.prefixSum[idx]
		rem := float64(suffixSum-int64(k)*int64(nRem)) / float64(nRem)
		d.erCache[k] = rem
	}

	d.dirty = false
	log.Debug().Msgf("[ERRDist] rebuilt: n=%d, maxStep=%d", n, maxStep)
}
