/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: EmpiricalDist for ERR scheduler — E[rem|curStep] computation.
 * Create: 2026-08-25
 */

package gs

import (
	"container/list"
	"sort"
	"sync"
	"sync/atomic"
	"time"
)

// distSnapshot is an immutable, pre-computed view of an EmpiricalDist.
// Readers access it via atomic pointer load — no locks needed.
// A Refresh() call builds a new snapshot and atomically swaps it in.
type distSnapshot struct {
	sortedVals []int
	erCache    map[int]float64
	totalCount int
}

func (s *distSnapshot) ExpectedRemaining(curStep int) float64 {
	if s == nil || len(s.sortedVals) == 0 {
		return 0.0
	}
	// curStep beyond every observed completion misses the cache and yields
	// 0.0 — the intended extrapolation: such a session has progressed past
	// all known completion points, so it inherits E[rem|k=maxStep]=0 (the
	// highest priority), consistent with MEJ's -k monotonicity.
	return s.erCache[curStep]
}

func (s *distSnapshot) TotalCount() int {
	if s == nil {
		return 0
	}
	return s.totalCount
}

func (s *distSnapshot) Median() int {
	if s == nil || len(s.sortedVals) == 0 {
		return 0
	}
	return s.sortedVals[len(s.sortedVals)/2]
}

type distEntry struct {
	sessionID string // "" for seeded/anonymous (immutable)
	steps     int
}

// EmpiricalDist maintains an empirical distribution of session step counts.
// Per-session single-record semantics: one session_id contributes at most ONE
// entry. Seeded entries (from SeedFromCounts) are immutable.
// When windowSize > 0, a FIFO sliding window bounds memory usage.
type EmpiricalDist struct {
	mu            sync.Mutex // protects rawValues, sessionValues and gen
	windowSize    int
	rawValues     *list.List
	sessionValues map[string]*list.Element
	dirty         bool
	// gen advances on every data mutation. A Refresh that copied data at gen G
	// skips its Store if gen moved past G, so a slow stale build can never
	// overwrite a fresher snapshot published meanwhile.
	gen           uint64
	snapshot      atomic.Pointer[distSnapshot]
	lastRefreshNs int64 // protected by d.mu
}

const refreshCooldownNs = int64(time.Second)

func NewEmpiricalDist(windowSize int) *EmpiricalDist {
	return &EmpiricalDist{
		windowSize:    windowSize,
		rawValues:     list.New(),
		sessionValues: make(map[string]*list.Element),
	}
}

// Snapshot returns the current immutable snapshot. Lock-free.
func (d *EmpiricalDist) Snapshot() *distSnapshot {
	return d.snapshot.Load()
}

// Refresh builds a new snapshot from rawValues and atomically swaps it in.
// Call after AddValue/SeedFromCounts to publish fresh E[rem|k] tables.
// Readers may briefly see the previous snapshot until the swap completes.
// If data mutated since the copy was taken, the Store is skipped: a
// concurrent Refresh already has fresher data and will publish it.
func (d *EmpiricalDist) Refresh() {
	d.mu.Lock()
	if !d.dirty {
		d.mu.Unlock()
		return
	}
	d.dirty = false

	n := d.rawValues.Len()
	if n == 0 {
		d.mu.Unlock()
		d.snapshot.Store(&distSnapshot{
			erCache: make(map[int]float64),
		})
		return
	}

	steps := make([]int, 0, n)
	for e := d.rawValues.Front(); e != nil; e = e.Next() {
		steps = append(steps, e.Value.(distEntry).steps)
	}
	genAtCopy := d.gen
	d.mu.Unlock()

	// Build outside the lock
	sort.Ints(steps)
	prefixSum := make([]int64, n+1)
	for i := 0; i < n; i++ {
		prefixSum[i+1] = prefixSum[i] + int64(steps[i])
	}
	erCache := make(map[int]float64, steps[n-1])
	maxStep := steps[n-1]
	for curStep := 1; curStep <= maxStep; curStep++ {
		idx := sort.SearchInts(steps, curStep)
		nRem := n - idx
		if nRem == 0 {
			erCache[curStep] = 0.0
			continue
		}
		suffixSum := prefixSum[n] - prefixSum[idx]
		erCache[curStep] = float64(suffixSum-int64(curStep)*int64(nRem)) / float64(nRem)
	}

	d.mu.Lock()
	stale := d.gen != genAtCopy
	d.mu.Unlock()
	if stale {
		return
	}

	d.snapshot.Store(&distSnapshot{
		sortedVals: steps,
		erCache:    erCache,
		totalCount: n,
	})
}

// MaybeRefresh rate-limits snapshot rebuilds to once per refreshCooldown.
// AddValue always writes under lock; only the snapshot rebuild is deferred.
func (d *EmpiricalDist) MaybeRefresh() {
	d.mu.Lock()
	now := time.Now().UnixNano()
	if now-d.lastRefreshNs < refreshCooldownNs {
		d.mu.Unlock()
		return
	}
	d.lastRefreshNs = now
	d.mu.Unlock()
	d.Refresh()
}

// evictFront removes the oldest entry from rawValues.
// Caller must hold d.mu.
func (d *EmpiricalDist) evictFront() {
	front := d.rawValues.Front()
	if front == nil {
		return
	}
	if entry, ok := front.Value.(distEntry); ok && entry.sessionID != "" {
		if d.sessionValues[entry.sessionID] == front {
			delete(d.sessionValues, entry.sessionID)
		}
	}
	d.rawValues.Remove(front)
}

// maxTrackedStep bounds the step values entering the distribution: an
// outlier session (miscounted or pathological) would otherwise inflate the
// erCache key space to O(maxStep) on every snapshot rebuild.
const maxTrackedStep = 1000

func (d *EmpiricalDist) AddValue(sessionID string, steps int) {
	if steps <= 0 || steps > maxTrackedStep {
		return
	}
	d.mu.Lock()
	if oldElem, ok := d.sessionValues[sessionID]; ok && oldElem != nil {
		d.rawValues.Remove(oldElem)
	}
	d.rawValues.PushBack(distEntry{sessionID: sessionID, steps: steps})
	if d.windowSize > 0 && d.rawValues.Len() > d.windowSize {
		d.evictFront()
	}
	if sessionID != "" {
		d.sessionValues[sessionID] = d.rawValues.Back()
	}
	d.dirty = true
	d.gen++
	d.mu.Unlock()
}

func (d *EmpiricalDist) SeedFromCounts(counts map[int]int) {
	if len(counts) == 0 {
		return
	}
	d.mu.Lock()
	for steps, cnt := range counts {
		if steps <= 0 || steps > maxTrackedStep || cnt <= 0 {
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
	d.gen++
	d.mu.Unlock()
	d.Refresh()
}

func (d *EmpiricalDist) ExpectedRemaining(curStep int) float64 {
	return d.snapshot.Load().ExpectedRemaining(curStep)
}

func (d *EmpiricalDist) TotalCount() int {
	return d.snapshot.Load().TotalCount()
}

// LiveCount reports the current observation count directly from the raw
// window, bypassing the snapshot refresh cooldown.
func (d *EmpiricalDist) LiveCount() int {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.rawValues.Len()
}

func (d *EmpiricalDist) Median() int {
	return d.snapshot.Load().Median()
}
