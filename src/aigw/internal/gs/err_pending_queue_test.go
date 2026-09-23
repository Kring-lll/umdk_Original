/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: PendingQueue test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestPendingQueueAddAndPick(t *testing.T) {
	q := NewPendingQueue(10)

	q.Add(&PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 5})
	q.Add(&PendingRequest{ReqID: "r2", SessionID: "s2", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r3", SessionID: "s3", CurStep: 3})

	// Peek should return highest priority (lowest CurStep value) without removing
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked == nil {
		t.Fatal("Peek returned nil")
	}
	if picked.ReqID != "r2" {
		t.Errorf("Peek ReqID = %s, want 'r2' (CurStep=1)", picked.ReqID)
	}
	q.Remove(picked.ReqID)

	picked = q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.ReqID != "r3" {
		t.Errorf("Peek ReqID = %s, want 'r3' (K=3)", picked.ReqID)
	}
	q.Remove(picked.ReqID)

	if q.Size() != 1 {
		t.Errorf("Size after 2 peek+remove = %d, want 1", q.Size())
	}
}

func TestPendingQueuePickEmpty(t *testing.T) {
	q := NewPendingQueue(10)
	if picked := q.Peek(func(r *PendingRequest) int { return r.CurStep }); picked != nil {
		t.Errorf("Peek on empty queue returned %v, want nil", picked)
	}
}

func TestPendingQueueIsFull(t *testing.T) {
	q := NewPendingQueue(2)
	q.Add(&PendingRequest{ReqID: "r1", CurStep: 1})
	if q.IsFull() {
		t.Error("Queue should not be full with 1 item")
	}
	q.Add(&PendingRequest{ReqID: "r2", CurStep: 2})
	if !q.IsFull() {
		t.Error("Queue should be full with 2 items (maxSize=2)")
	}
}

func TestPendingQueueFullReturnsError(t *testing.T) {
	q := NewPendingQueue(1)
	q.Add(&PendingRequest{ReqID: "r1", CurStep: 5})
	err := q.Add(&PendingRequest{ReqID: "r2", CurStep: 2})
	if err != ErrQueueFull {
		t.Errorf("Add when full returned %v, want ErrQueueFull", err)
	}
}

func TestPendingQueueAddRejectsDuplicateReqID(t *testing.T) {
	q := NewPendingQueue(10)
	err := q.Add(&PendingRequest{ReqID: "r1", CurStep: 1})
	assert.NoError(t, err)

	// Duplicate ReqID must be rejected, not silently overwrite
	err = q.Add(&PendingRequest{ReqID: "r1", CurStep: 5})
	assert.ErrorIs(t, err, ErrDuplicateReqID)

	if q.Size() != 1 {
		t.Errorf("Size after duplicate = %d, want 1", q.Size())
	}
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.CurStep != 1 {
		t.Errorf("CurStep after duplicate = %d, want 1 (original must survive)", picked.CurStep)
	}
}

func TestPendingQueuePickTieBreakByEnqueueTime(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", CurStep: 5})
	q.Add(&PendingRequest{ReqID: "r2", CurStep: 5})

	// Same CurStep: oldest (r1) should be picked first
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.ReqID != "r1" {
		t.Errorf("Tie-break picked %s, want 'r1' (oldest)", picked.ReqID)
	}
}

func TestPendingQueueRemoveBySession(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r2", SessionID: "s2", CurStep: 2})

	q.RemoveBySession("s1")
	if q.Size() != 1 {
		t.Errorf("Size after remove = %d, want 1", q.Size())
	}
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.SessionID != "s2" {
		t.Errorf("Remaining session = %s, want 's2'", picked.SessionID)
	}
}

func TestPendingQueueRemoveBySessionMultipleSameSession(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r2", SessionID: "s1", CurStep: 2})
	q.Add(&PendingRequest{ReqID: "r3", SessionID: "s2", CurStep: 3})
	q.Add(&PendingRequest{ReqID: "r4", SessionID: "s1", CurStep: 4})

	q.RemoveBySession("s1")
	if q.Size() != 1 {
		t.Errorf("Size after remove = %d, want 1 (only s2 should remain)", q.Size())
	}
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.SessionID != "s2" {
		t.Errorf("Remaining session = %s, want 's2'", picked.SessionID)
	}
}

func TestPendingQueueEvictLowestStep(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 5})
	q.Add(&PendingRequest{ReqID: "r2", SessionID: "s2", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r3", SessionID: "s3", CurStep: 3})

	victim := q.EvictLowestStep()
	if victim == nil {
		t.Fatal("EvictLowestStep returned nil")
	}
	if victim.ReqID != "r2" {
		t.Errorf("Evicted ReqID = %s, want 'r2' (CurStep=1)", victim.ReqID)
	}
	if q.Size() != 2 {
		t.Errorf("Size after evict = %d, want 2", q.Size())
	}
}

func TestPendingQueuePeekLowestStep(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", CurStep: 5})
	q.Add(&PendingRequest{ReqID: "r2", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r3", CurStep: 3})

	// Peek should return CurStep=1 without removing
	peeked := q.PeekLowestStep()
	assert.NotNil(t, peeked)
	assert.Equal(t, "r2", peeked.ReqID)
	assert.Equal(t, 1, peeked.CurStep)

	// Queue should be unchanged after peek
	assert.Equal(t, 3, q.Size())

	// Peek again — same result
	peeked2 := q.PeekLowestStep()
	assert.Equal(t, "r2", peeked2.ReqID)
	assert.Equal(t, 3, q.Size())
}

func TestPendingQueuePeekLowestStepEmpty(t *testing.T) {
	q := NewPendingQueue(10)
	assert.Nil(t, q.PeekLowestStep())
}

func TestPendingQueueDrainAll(t *testing.T) {
	q := NewPendingQueue(10)
	q.Add(&PendingRequest{ReqID: "r1", CurStep: 1})
	q.Add(&PendingRequest{ReqID: "r2", CurStep: 2})
	q.Add(&PendingRequest{ReqID: "r3", CurStep: 3})

	all := q.DrainAll()
	if len(all) != 3 {
		t.Errorf("DrainAll returned %d items, want 3", len(all))
	}
	if q.Size() != 0 {
		t.Errorf("Size after drain = %d, want 0", q.Size())
	}
}

func TestPendingQueueAddDuplicateReqID(t *testing.T) {
	q := NewPendingQueue(10)

	req1 := &PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 1}
	err := q.Add(req1)
	assert.NoError(t, err)

	// Same ReqID — should be rejected, not silently overwrite
	req2 := &PendingRequest{ReqID: "r1", SessionID: "s2", CurStep: 2}
	err = q.Add(req2)
	assert.ErrorIs(t, err, ErrDuplicateReqID)

	// Original request must still be in the queue
	if q.Size() != 1 {
		t.Errorf("Size after duplicate add = %d, want 1", q.Size())
	}
	picked := q.Peek(func(r *PendingRequest) int { return r.CurStep })
	if picked.SessionID != "s1" {
		t.Errorf("Remaining session = %s, want 's1' (original must survive)", picked.SessionID)
	}
}

// TestPendingQueueQueueFullEvictStrategy tests the dispatchWithQueue eviction
// strategy at the PendingQueue level: PeekLowestStep + comparison logic.
func TestPendingQueueQueueFullEvictStrategy(t *testing.T) {
	q := NewPendingQueue(3)

	// Fill queue: CurStep 5, 3, 1 (lowest = 1)
	q.Add(&PendingRequest{ReqID: "r1", SessionID: "s1", CurStep: 5})
	q.Add(&PendingRequest{ReqID: "r2", SessionID: "s2", CurStep: 3})
	q.Add(&PendingRequest{ReqID: "r3", SessionID: "s3", CurStep: 1})
	assert.True(t, q.IsFull())

	// New request with CurStep=2: 2 > 1 (lowest), so victim (CurStep=1) should be evicted
	victim := q.PeekLowestStep()
	assert.NotNil(t, victim)
	assert.Equal(t, 1, victim.CurStep)
	assert.Equal(t, "s3", victim.SessionID)

	// Simulate: new CurStep=2 > victim.CurStep=1 → evict victim, add new
	evicted := q.EvictLowestStep()
	assert.Equal(t, "s3", evicted.SessionID)
	assert.Equal(t, 2, q.Size())

	q.Add(&PendingRequest{ReqID: "r4", SessionID: "s4", CurStep: 2})
	assert.Equal(t, 3, q.Size())

	// New request with CurStep=1: 1 <= 2 (lowest now = 2) → should reject new, keep queue
	victim2 := q.PeekLowestStep()
	assert.Equal(t, 2, victim2.CurStep)
	// CurStep=1 <= victim.CurStep=2 → reject new request (simulated by NOT evicting)
	assert.True(t, 1 <= victim2.CurStep, "new CurStep=1 should be <= victim CurStep=2, reject new")
}
