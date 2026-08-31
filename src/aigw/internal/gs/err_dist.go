/*
 * SPDX-License-Identifier: MIT
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
// Per-session single-record semantics: one session_id contributes at most ONE
// entry. Seeded entries (from SeedFromCounts) are immutable.
// When windowSize > 0, a FIFO sliding window bounds memory usage.
type distEntry struct {
	sessionID string // "" for seeded/anonymous (immutable)
	steps     int
}

type EmpiricalDist struct {
	mu            sync.RWMutex
	windowSize    int
	rawValues     *list.List
	sessionValues map[string]*list.Element
	dirty         bool

	sortedVals []int
	prefixSum  []int64
	erCache    map[int]float64
	totalCount int
}

func NewEmpiricalDist(windowSize int) *EmpiricalDist {
	return &EmpiricalDist{
		windowSize:    windowSize,
		rawValues:     list.New(),
		sessionValues: make(map[string]*list.Element),
		erCache:       make(map[int]float64),
		dirty:         true,
	}
}

// evictFront removes the front (oldest) entry. Caller must hold d.mu.
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

// AddValue records (or updates) a session's step count.
// If the session already has an entry, the old entry is replaced.
func (d *EmpiricalDist) AddValue(sessionID string, steps int) {
	if steps <= 0 {
		return
	}
	d.mu.Lock()
	defer d.mu.Unlock()

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
}

// SeedFromCounts bulk-loads from a {steps: count} map.
// Seeded entries are anonymous (sessionID=""): immutable, never replaced.
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
// Returns 0.0 when k is beyond the distribution's support.
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

// Median returns the 50th-percentile step count (0 if empty).
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

	d.prefixSum = make([]int64, n+1)
	for i := 0; i < n; i++ {
		d.prefixSum[i+1] = d.prefixSum[i] + int64(d.sortedVals[i])
	}

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
		d.erCache[k] = float64(suffixSum-int64(k)*int64(nRem)) / float64(nRem)
	}

	d.dirty = false
	log.Debug().Msgf("[ERRDist] rebuilt: n=%d, maxStep=%d", n, maxStep)
}
