/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR algorithms test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import "testing"

func TestMejAlgPriority(t *testing.T) {
	alg := NewMejAlg()
	if alg.Priority("", 1, "") != -1 {
		t.Errorf("MEJ priority for curStep=1 should be -1, got %v", alg.Priority("", 1, ""))
	}
	if alg.Priority("", 10, "") != -10 {
		t.Errorf("MEJ priority for curStep=10 should be -10, got %v", alg.Priority("", 10, ""))
	}
	if alg.Priority("", 0, "") != -1 {
		t.Errorf("MEJ priority for curStep=0 should default to curStep=1 (-1), got %v", alg.Priority("", 0, ""))
	}
}

func TestMejAlgName(t *testing.T) {
	if NewMejAlg().Name() != "mej" {
		t.Error("MEJ name should be 'mej'")
	}
}

func TestErrAlgPriorityWithNilDist(t *testing.T) {
	alg := NewErrAlg(nil)
	if alg.Priority("", 5, "") != -5 {
		t.Errorf("ERR with nil dist should fall back to MEJ (-5), got %v", alg.Priority("", 5, ""))
	}
}

func TestErrAlgPriorityWithDist(t *testing.T) {
	dist := NewEmpiricalDist(0)
	dist.SeedFromCounts(map[int]int{5: 1, 10: 1})
	alg := NewErrAlg(dist)

	// E[rem|curStep] for curStep=1 with steps {5,10}: ((5-1)+(10-1))/2 = 6.5
	pri := alg.Priority("", 1, "")
	if pri != 6.5 {
		t.Errorf("E[rem|1] for {5,10} should be 6.5, got %v", pri)
	}

	// curStep beyond distribution support → 0.0
	pri = alg.Priority("", 11, "")
	if pri != 0.0 {
		t.Errorf("E[rem|11] for {5,10} should be 0.0, got %v", pri)
	}
}

func TestErrAlgName(t *testing.T) {
	if NewErrAlg(nil).Name() != "err" {
		t.Error("ERR name should be 'err'")
	}
}

func TestCatErrAlgPriorityWithoutClassifier(t *testing.T) {
	alg := NewCatErrAlg(nil, nil, nil)
	if alg.Priority("", 3, "hello") != -3 {
		t.Errorf("CatERR without classifier should fall back to MEJ (-3), got %v", alg.Priority("", 3, "hello"))
	}
}

func TestCatErrAlgName(t *testing.T) {
	if NewCatErrAlg(nil, nil, nil).Name() != "caterr" {
		t.Error("CatERR name should be 'caterr'")
	}
}

func TestFifoAlgPriority(t *testing.T) {
	alg := NewFifoAlg()
	// FIFO priority is always 0 regardless of session or curStep
	if alg.Priority("sess-1", 1, "hello") != 0 {
		t.Error("FIFO priority should be 0")
	}
	if alg.Priority("sess-2", 100, "world") != 0 {
		t.Error("FIFO priority should be 0 regardless of curStep")
	}
	if alg.PriorityWith(nil, "sess-1", 5, "hello") != 0 {
		t.Error("FIFO PriorityWith should be 0")
	}
}

func TestFifoAlgName(t *testing.T) {
	if NewFifoAlg().Name() != "fifo" {
		t.Error("FIFO name should be 'fifo'")
	}
}

func TestFifoPendingQueueOrdering(t *testing.T) {
	// With FIFO algorithm, all priorities are 0 → Peek's tie-break
	// (EnqueueTime, oldest first) becomes the sole ordering criterion.
	m := &GlobalSchedulerManager{
		errScheduler: NewErrSchedulerManager(ErrSchedulerConfig{
			SchedulerType: "fifo",
		}),
	}
	m.pendingQueue = NewPendingQueue(10)
	m.errInFlight = make(map[string]errInFlightEntry)
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.capacityProvider.OnDispatched() // fill the only slot

	// Enqueue in order: req-1, req-2, req-3
	m.dispatchMu.Lock()
	m.pendingQueue.Add(&PendingRequest{ReqID: "req-1", SessionID: "s1", CurStep: 5, Msg: nil})
	m.pendingQueue.Add(&PendingRequest{ReqID: "req-2", SessionID: "s2", CurStep: 1, Msg: nil})
	m.pendingQueue.Add(&PendingRequest{ReqID: "req-3", SessionID: "s3", CurStep: 3, Msg: nil})
	m.dispatchMu.Unlock()

	// Peek should return req-1 (earliest EnqueueTime) even though
	// req-2 has the lowest curStep (which would win under MEJ/ERR)
	snap := m.errScheduler.AlgoSnapshot()
	m.dispatchMu.Lock()
	pr := m.pendingQueue.Peek(func(r *PendingRequest) int {
		return int(m.errScheduler.PriorityWith(snap, r.SessionID, r.CurStep, ""))
	})
	m.dispatchMu.Unlock()
	if pr == nil || pr.ReqID != "req-1" {
		t.Errorf("FIFO Peek should return req-1 (earliest), got %v", pr)
	}
}
