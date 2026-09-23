/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: EmpiricalDist test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestEmpiricalDistExpectedRemaining(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.SeedFromCounts(map[int]int{5: 1, 10: 1})

	// E[rem|1] = ((5-1)+(10-1))/2 = 6.5
	if er := dist.ExpectedRemaining(1); er != 6.5 {
		t.Errorf("E[rem|1] = %v, want 6.5", er)
	}

	// E[rem|5] = (0+(10-5))/2 = 2.5
	if er := dist.ExpectedRemaining(5); er != 2.5 {
		t.Errorf("E[rem|5] = %v, want 2.5", er)
	}

	// E[rem|6] = (10-6)/1 = 4.0
	if er := dist.ExpectedRemaining(6); er != 4.0 {
		t.Errorf("E[rem|6] = %v, want 4.0", er)
	}

	// curStep beyond support → 0.0
	if er := dist.ExpectedRemaining(11); er != 0.0 {
		t.Errorf("E[rem|11] = %v, want 0.0", er)
	}
}

func TestEmpiricalDistEmpty(t *testing.T) {
	dist := NewEmpiricalDist(0)
	if er := dist.ExpectedRemaining(1); er != 0.0 {
		t.Errorf("E[rem|1] on empty dist = %v, want 0.0", er)
	}
	if dist.TotalCount() != 0 {
		t.Errorf("TotalCount on empty dist = %d, want 0", dist.TotalCount())
	}
	if dist.Median() != 0 {
		t.Errorf("Median on empty dist = %d, want 0", dist.Median())
	}
}

func TestEmpiricalDistTotalCount(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.SeedFromCounts(map[int]int{3: 2, 7: 3})
	if dist.TotalCount() != 5 {
		t.Errorf("TotalCount = %d, want 5", dist.TotalCount())
	}
}

func TestEmpiricalDistMedian(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.SeedFromCounts(map[int]int{3: 1, 5: 1, 7: 1, 10: 1, 20: 1})
	// sorted: [3, 5, 7, 10, 20], median = 7
	if m := dist.Median(); m != 7 {
		t.Errorf("Median = %d, want 7", m)
	}
}

func TestEmpiricalDistAddValueReplaces(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.AddValue("session-1", 5)
	dist.Refresh()
	if dist.TotalCount() != 1 {
		t.Errorf("TotalCount after first add = %d, want 1", dist.TotalCount())
	}

	// Same session, new step count → replaces, not appends
	dist.AddValue("session-1", 10)
	dist.Refresh()
	if dist.TotalCount() != 1 {
		t.Errorf("TotalCount after replace = %d, want 1", dist.TotalCount())
	}

	// Different session → appends
	dist.AddValue("session-2", 3)
	dist.Refresh()
	if dist.TotalCount() != 2 {
		t.Errorf("TotalCount after new session = %d, want 2", dist.TotalCount())
	}
}

func TestEmpiricalDistAddValueZeroSteps(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.AddValue("session-1", 0)
	dist.Refresh()
	if dist.TotalCount() != 0 {
		t.Errorf("TotalCount after adding 0 steps = %d, want 0", dist.TotalCount())
	}
}

func TestEmpiricalDistSlidingWindow(t *testing.T) {
	dist := NewEmpiricalDist(3)
	dist.AddValue("s1", 5)
	dist.Refresh()
	dist.AddValue("s2", 10)
	dist.Refresh()
	dist.AddValue("s3", 15)
	dist.Refresh()

	if dist.TotalCount() != 3 {
		t.Errorf("TotalCount = %d, want 3", dist.TotalCount())
	}

	// Adding a 4th should evict the oldest (s1)
	dist.AddValue("s4", 20)
	dist.Refresh()
	if dist.TotalCount() != 3 {
		t.Errorf("TotalCount after window eviction = %d, want 3", dist.TotalCount())
	}
}

func TestMaybeRefreshCooldown(t *testing.T) {
	d := NewEmpiricalDist(0)
	d.AddValue("s1", 5)
	d.AddValue("s2", 10)

	// First MaybeRefresh should trigger Refresh (lastRefreshNs was 0)
	d.MaybeRefresh()
	assert.NotNil(t, d.Snapshot())
	assert.Equal(t, 2, d.Snapshot().TotalCount())

	// Second MaybeRefresh within cooldown should NOT refresh
	// Add new data that would change the snapshot
	d.AddValue("s3", 15)
	d.MaybeRefresh()
	// Snapshot should still show 2 (old data), not 3
	assert.Equal(t, 2, d.Snapshot().TotalCount(), "MaybeRefresh within cooldown should not refresh")

	// Force refresh to verify new data is there
	d.Refresh()
	assert.Equal(t, 3, d.Snapshot().TotalCount())
}

func TestMaybeRefreshConcurrentSafe(t *testing.T) {
	d := NewEmpiricalDist(0)

	done := make(chan struct{}, 10)
	for i := 0; i < 10; i++ {
		go func(n int) {
			d.AddValue("session-"+string(rune('a'+n)), n+1)
			d.MaybeRefresh()
			done <- struct{}{}
		}(i)
	}
	for i := 0; i < 10; i++ {
		<-done
	}

	// After all goroutines, data should be intact
	d.Refresh()
	assert.Equal(t, 10, d.Snapshot().TotalCount())
}

func TestEmpiricalDistSnapshot(t *testing.T) {
	d := NewEmpiricalDist(0)
	d.AddValue("s1", 5)
	d.AddValue("s2", 10)
	d.AddValue("s3", 3)
	d.Refresh()

	snap := d.Snapshot()
	assert.NotNil(t, snap)
	assert.Equal(t, 3, snap.TotalCount())
	assert.Equal(t, 3, len(snap.sortedVals))
	// sorted: [3, 5, 10]
	assert.Equal(t, 3, snap.sortedVals[0])
	assert.Equal(t, 5, snap.sortedVals[1])
	assert.Equal(t, 10, snap.sortedVals[2])

	// E[rem|1] = ((3-1)+(5-1)+(10-1))/3 = 5.0
	assert.InDelta(t, 5.0, snap.ExpectedRemaining(1), 0.001)
	// E[rem|3] = ((3-3)+(5-3)+(10-3))/3 = 3.0
	assert.InDelta(t, 3.0, snap.ExpectedRemaining(3), 0.001)
	// E[rem|5] = ((5-5)+(10-5))/2 = 2.5
	assert.InDelta(t, 2.5, snap.ExpectedRemaining(5), 0.001)
	// E[rem|10] = 0 (max step, nothing remaining)
	assert.InDelta(t, 0.0, snap.ExpectedRemaining(10), 0.001)
	// E[rem|100] = 0 (beyond distribution)
	assert.InDelta(t, 0.0, snap.ExpectedRemaining(100), 0.001)

	// Snapshot is immutable — adding more data doesn't change old snapshot
	d.AddValue("s4", 20)
	assert.Equal(t, 3, snap.TotalCount(), "old snapshot must be immutable")
	d.Refresh()
	assert.Equal(t, 4, d.Snapshot().TotalCount())
}

func TestEmpiricalDistSnapshotEmpty(t *testing.T) {
	d := NewEmpiricalDist(0)

	// Before any Refresh, snapshot is nil
	assert.Nil(t, d.Snapshot())

	// After Refresh (empty data), snapshot is non-nil but empty
	d.dirty = true // force refresh
	d.Refresh()

	snap := d.Snapshot()
	assert.NotNil(t, snap)
	assert.Equal(t, 0, snap.TotalCount())
	assert.Equal(t, 0.0, snap.ExpectedRemaining(1))
}

func TestEmpiricalDistEvictFront(t *testing.T) {
	d := NewEmpiricalDist(3)
	d.AddValue("s1", 5)
	d.AddValue("s2", 10)
	d.AddValue("s3", 3)

	// Window is full (3 items). Add 4th → evict oldest (s1)
	d.AddValue("s4", 20)
	d.Refresh()

	snap := d.Snapshot()
	assert.Equal(t, 3, snap.TotalCount())
	// s1 (steps=5) should be evicted; remaining: s2=10, s3=3, s4=20
	// sorted: [3, 10, 20]
	assert.Equal(t, 3, snap.sortedVals[0])
	assert.Equal(t, 10, snap.sortedVals[1])
	assert.Equal(t, 20, snap.sortedVals[2])
}
