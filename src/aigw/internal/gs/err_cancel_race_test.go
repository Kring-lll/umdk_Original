/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for the cancel-vs-dispatch race (Deliver/Abandon
 * rendezvous on ControlMessage) and the queue-head ctx-dead k rollback.
 * Create: 2026-09-08
 */

package gs

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"

	"huawei.com/aigw/internal/stats"
)

// TestAbandonedDispatchUndoesAccounting: the requester gives up in the window
// between the dispatch decision (books taken: in-flight slot, errInFlight
// registry, LB AddRequest) and the result delivery. Deliver must refuse the
// write and dispatchScheduled must roll every book back, so the slot serves
// the next queued request instead of leaking to the 660s zombie scan.
func TestAbandonedDispatchUndoesAccounting(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-race-a", "model", "p") // k=1

	// Request A: requester already abandoned (ctx fired inside the race
	// window, after the queue-head ctx check passed — simulated by a nil
	// ReqCTX so the check is skipped).
	rspA := make(chan interface{}, 1)
	msgA := &ControlMessage{
		Request: &ScheduleRequestMsg{
			Request:   &LlmRequest{ReqId: "race-a"},
			SessionID: "sess-race-a",
		},
		Response: rspA,
	}
	msgA.Abandon()
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "race-a",
		SessionID: "sess-race-a",
		CurStep:   1,
		Msg:       msgA,
	}))

	// Request B: normal, queued behind A.
	rspB := make(chan interface{}, 1)
	msgB := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "race-b"}},
		Response: rspB,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "race-b",
		SessionID: "sess-race-b",
		CurStep:   1,
		Msg:       msgB,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	// A's dispatch was undone: no registry entry, no response delivered,
	// session deleted (in-flight abort semantics).
	m.dispatchMu.Lock()
	_, aInFlight := m.errInFlight["race-a"]
	_, bInFlight := m.errInFlight["race-b"]
	m.dispatchMu.Unlock()
	assert.False(t, aInFlight, "abandoned dispatch must leave no in-flight entry")
	assert.True(t, bInFlight, "the next request owns the slot")
	select {
	case out := <-rspA:
		t.Fatalf("abandoned requester must not receive a result, got %v", out)
	default:
	}
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-race-a"),
		"k rolled back, session survives for the retry")

	// The freed slot immediately serves B — no leak until the zombie scan.
	select {
	case out := <-rspB:
		s, ok := out.(*SuggestionResultMsg)
		if !ok || s.PrefillUrl == "" {
			t.Fatalf("request B dispatched with unexpected result: %v", out)
		}
	default:
		t.Fatal("request B must take over the slot released by the undone dispatch")
	}
	assert.Equal(t, 1, m.ErrInFlightCount())
	assert.Equal(t, 0, m.pendingQueue.Size())
}

// TestAbandonedDispatchAnonymous: the same race with no session — the undo
// path must not touch session state (no panic, no ghost session).
func TestAbandonedDispatchAnonymous(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "anon-race"}},
		Response: rsp,
	}
	msg.Abandon()
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:   "anon-race",
		CurStep: 1,
		Msg:     msg,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	assert.Equal(t, 0, m.ErrInFlightCount(), "anonymous abandoned dispatch must leave no in-flight entry")
	select {
	case out := <-rsp:
		t.Fatalf("abandoned requester must not receive a result, got %v", out)
	default:
	}
}

// TestAbandonDrainsDispatchedSuggestion: the requester's context fires AFTER
// the dispatcher delivered. Abandon must hand the buffered suggestion back so
// the handler can release the in-flight accounting with the instance URL —
// nobody else will observe the completion.
func TestAbandonDrainsDispatchedSuggestion(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-drain", "model", "p")

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "drain-1"}},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "drain-1",
		SessionID: "sess-drain",
		CurStep:   1,
		Msg:       msg,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
	assert.Equal(t, 1, m.ErrInFlightCount())

	// Handler side: ctx fired, Abandon drains the suggestion that landed
	// concurrently with the cancellation.
	sug, ok := msg.Abandon().(*SuggestionResultMsg)
	if !ok || sug.PrefillUrl == "" {
		t.Fatalf("Abandon must drain the delivered suggestion, got %v", sug)
	}
	// The handler then releases with the dispatch target, as in
	// scheduleRequest's ctx.Done branch.
	m.ErrOnCancel("sess-drain", "drain-1", sug.PrefillUrl, sug.DecodeUrl)

	assert.Equal(t, 0, m.ErrInFlightCount(), "drained suggestion must be released via ErrOnCancel")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-drain"),
		"in-flight abort rolls k back, the session survives for the retry")
}

// TestQueueHeadCtxDeadRollsBackIteration: the dispatch loop notices the
// queue-head request's ctx is dead and removes it before the handler-side
// three-state cleanup can see it. The k rollback must happen at the removal
// site — exactly once — or the retry re-enters at k+1 (k=1,1,1 spiral class).
func TestQueueHeadCtxDeadRollsBackIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-ctx", "model", "p") // k=1
	m.errScheduler.OnRequest("sess-ctx", "model", "p") // k=2

	deadCtx, cancel := context.WithCancel(context.Background())
	cancel()
	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request: &ScheduleRequestMsg{
			Request: &LlmRequest{ReqId: "ctx-2"},
			ReqCTX:  deadCtx,
		},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "ctx-2",
		SessionID: "sess-ctx",
		CurStep:   2,
		Msg:       msg,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-ctx"),
		"the dead-ctx queue removal must roll back the step it booked")
	assert.Equal(t, 2, m.errScheduler.OnRequest("sess-ctx", "model", "p"),
		"the retry re-enters at the same step number")
	assert.Equal(t, 0, m.ErrInFlightCount(), "nothing was dispatched")
	select {
	case out := <-rsp:
		if out != ErrNoInstance {
			t.Fatalf("expected ErrNoInstance for the dead request, got %v", out)
		}
	default:
		t.Fatal("the dead request must be answered")
	}
	assert.Equal(t, 0, m.pendingQueue.Size(), "queue must be drained")
}

// dispatchErr-side k accounting: every path that fails an attempt AFTER
// OnRequest (k++) but BEFORE dispatch must roll the k back exactly once —
// a failed attempt must not count as a step, and failures must not earn
// priority (both MEJ -k and ERR E[rem|k] rank higher k BETTER).

type emptyScheduleLB struct{}

func (emptyScheduleLB) schedule(*ScheduleRequestMsg, *ScheduleOptions) *ScheduleResult {
	return &ScheduleResult{}
}

type failingAddMetric struct{ starvationTestMetric }

func (failingAddMetric) AddRequest(*LlmRequest, *InstanceContext) error {
	return fmt.Errorf("provider add failed")
}

// TestQueueFullRejectRollsBackIteration: the new request is rejected
// (victim has more progress) — its k++ must be undone, the retry re-enters
// at the same k, and the handler-side cleanup must not roll back a second
// time.
func TestQueueFullRejectRollsBackIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.pendingQueue = NewPendingQueue(1)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-rj", "model", "p") // k=1 earned

	victimRsp := make(chan interface{}, 1)
	m.pendingQueue.Add(&PendingRequest{
		ReqID:     "victim-1",
		SessionID: "sess-victim",
		CurStep:   5,
		Msg:       &ControlMessage{Request: &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "victim-1"}}, Response: victimRsp},
	})

	curStep := m.errScheduler.OnRequest("sess-rj", "model", "p") // k=2 (this attempt)
	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "rj-2"}, SessionID: "sess-rj"},
		Response: rsp,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msg, curStep)
	m.dispatchMu.Unlock()

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-rj"),
		"the rejected attempt must not count as a step")
	select {
	case out := <-rsp:
		assert.Equal(t, ErrQueueFull, out)
	default:
		t.Fatal("rejected request must be answered")
	}

	// The handler-side three-state cleanup runs afterwards; it must find no
	// queued entry and not decrement k a second time.
	m.ErrOnCancel("sess-rj", "rj-2", "", "")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-rj"),
		"exactly-once: handler-side cleanup must not roll back again")
	assert.Equal(t, 2, m.errScheduler.OnRequest("sess-rj", "model", "p"),
		"the retry re-enters at the same step number")
}

// TestQueueFullEvictionRollsBackVictimIteration: the evicted victim's k++
// (its queued attempt) must be undone — the victim's own retry re-enters at
// the same k.
func TestQueueFullEvictionRollsBackVictimIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.pendingQueue = NewPendingQueue(1)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	m.errScheduler.OnRequest("sess-victim", "model", "p") // k=1
	m.errScheduler.OnRequest("sess-victim", "model", "p") // k=2 (queued attempt)
	victimRsp := make(chan interface{}, 1)
	m.pendingQueue.Add(&PendingRequest{
		ReqID:     "victim-2",
		SessionID: "sess-victim",
		CurStep:   2,
		Msg:       &ControlMessage{Request: &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "victim-2"}}, Response: victimRsp},
	})

	// The new request has more progress than the victim (k=3 vs CurStep=2),
	// so it evicts the victim instead of being rejected.
	curStep := 3
	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "big-3"}, SessionID: "sess-big"},
		Response: rsp,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msg, curStep)
	m.dispatchMu.Unlock()

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-victim"),
		"the evicted attempt must not count as a step")
	select {
	case out := <-victimRsp:
		assert.Equal(t, ErrQueueFull, out)
	default:
		t.Fatal("evicted victim must be answered")
	}
	// The freed slot immediately serves the new request (capacity is free),
	// and the queue ends empty: victim evicted, new request dispatched.
	select {
	case out := <-rsp:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "new request takes the freed slot, got %v", out)
	default:
		t.Fatal("new request must be dispatched")
	}
	assert.Equal(t, 0, m.pendingQueue.Size())
}

// TestAddRequestFailureRollsBackIteration: dispatchScheduled fails at
// AddRequest (after the request left the queue, before OnDispatched) — the
// k++ must be undone here, and no in-flight slot may be taken.
func TestAddRequestFailureRollsBackIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = failingAddMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-af", "model", "p") // k=1 earned

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "af-2"}, SessionID: "sess-af"},
		Response: rsp,
	}
	m.dispatchErr(msg) // OnRequest → k=2, dispatch fails at AddRequest

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-af"),
		"the AddRequest-failed attempt must not count as a step")
	assert.Equal(t, 0, m.ErrInFlightCount(), "no slot was taken")
	select {
	case out := <-rsp:
		err, ok := out.(error)
		assert.True(t, ok, "request must be answered with an error, got %v", out)
		assert.Contains(t, err.Error(), "provider add failed")
	default:
		t.Fatal("failed request must be answered")
	}
	m.ErrOnCancel("sess-af", "af-2", "", "")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-af"), "exactly-once holds")
}

// TestDirectEmptyResultRollsBackIteration: passthrough mode (no queue), LB
// returns no instance — the attempt completed nothing, k rolls back.
func TestDirectEmptyResultRollsBackIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.pendingQueue = nil
	m.scheduler = emptyScheduleLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-dt", "model", "p") // k=1 earned

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "dt-2"}, SessionID: "sess-dt"},
		Response: rsp,
	}
	m.dispatchErr(msg) // OnRequest → k=2, LB empty → rollback

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-dt"),
		"the no-instance attempt must not count as a step")
	select {
	case out := <-rsp:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok, "expected empty suggestion, got %v", out)
		assert.Empty(t, s.PrefillUrl)
	default:
		t.Fatal("request must be answered")
	}
	m.ErrOnCancel("sess-dt", "dt-2", "", "")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-dt"), "exactly-once holds")
}

// TestRetryInheritsStarvationFloor: a session whose attempt starved 300s in
// the queue (client timeout) retries with a fresh request. The retry must
// re-enter ALREADY boosted — the 240s floor would otherwise restart from
// zero and the session would starve again (the k=1,1,1 death spiral's
// sibling: the boost-clock reset spiral).
func TestRetryInheritsStarvationFloor(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	// Attempt 1: queued 300s ago, never dispatched (all slots busy) → the
	// client's 300s budget fires → ErrOnCancel sees it queued → rollback +
	// 300s of carried wait.
	m.errScheduler.OnRequest("sess-rtry", "model", "p") // k=1
	rsp1 := make(chan interface{}, 1)
	msg1 := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "rtry-1"}, SessionID: "sess-rtry"},
		Response: rsp1,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "rtry-1", SessionID: "sess-rtry", CurStep: 1, Msg: msg1,
		EnqueueTime: time.Now().Add(-300 * time.Second),
	}))
	m.ErrOnCancel("sess-rtry", "rtry-1", "", "")

	// Contention setup: occupy the only slot so both candidates queue.
	m.capacityProvider.OnDispatched()

	// Attempt 2 (the retry, k rolled back to 1 → this attempt books k=2,
	// MEJ priority -2) enqueues fresh, competing against a fresh k=8
	// request (priority -8) that would otherwise always outrank it.
	rspR := make(chan interface{}, 1)
	msgR := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "rtry-2"}, SessionID: "sess-rtry"},
		Response: rspR,
	}
	rspB := make(chan interface{}, 1)
	msgB := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "fresh-8"}, SessionID: "sess-fresh"},
		Response: rspB,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msgR, m.errScheduler.OnRequest("sess-rtry", "model", "p"))
	// The competitor sits at step 8 (MEJ priority -8): without the carried
	// boost the retry (-2) loses to it.
	m.dispatchWithQueue(msgB, 8)
	assert.Equal(t, 2, m.pendingQueue.Size(), "both candidates queued while the slot is busy")

	// The slot frees: the starved retry must outrank the fresh k=8.
	m.capacityProvider.OnCompleted()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	select {
	case out := <-rspR:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "the starved retry must take the slot, got %v", out)
	default:
		t.Fatal("retry lost to a fresher request — carried starvation debt did not reach the boost floor")
	}
	select {
	case <-rspB:
		t.Fatal("fresh request must not be dispatched before the starved retry")
	default:
	}
}

// TestCarryRollsForwardAcrossAttempts: attempt 2 (carried 300s, boosted) can
// still starve if the slot never frees; its timeout must record the TOTAL
// starve-clock age (600s), not just this attempt's wait, so attempt 3 ranks
// ahead of every less-starved request in the boost tier.
func TestCarryRollsForwardAcrossAttempts(t *testing.T) {
	m := newErrTestGSM(t)
	carry := m.errScheduler.TakeQueueWaitCarry("no-such-session")
	assert.Equal(t, time.Duration(0), carry, "unknown session has no debt")

	m.errScheduler.OnRequest("sess-roll", "model", "p") // k=1
	m.errScheduler.OnQueueTimeout("sess-roll", 300*time.Second)
	assert.Equal(t, 300*time.Second, m.errScheduler.TakeQueueWaitCarry("sess-roll"),
		"debt is read-and-reset: first take returns it")
	assert.Equal(t, time.Duration(0), m.errScheduler.TakeQueueWaitCarry("sess-roll"),
		"second take returns zero")

	// Attempt 2 carries 300s in, starves another 300s → OnQueueTimeout
	// records StarveClock age (600s total).
	m.errScheduler.OnQueueTimeout("sess-roll", 600*time.Second)
	assert.Equal(t, 600*time.Second, m.errScheduler.TakeQueueWaitCarry("sess-roll"),
		"debt accumulates monotonically across attempts")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-roll"),
		"each queue timeout rolls k back to the same step")
}

// TestCarryClearedAfterSuccessfulDispatch: once a carried attempt dispatches,
// the debt is consumed — the session's next step starts with a clean floor.
func TestCarryClearedAfterSuccessfulDispatch(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	m.errScheduler.OnQueueTimeout("sess-ok", 300*time.Second)
	m.errScheduler.OnRequest("sess-ok", "model", "p") // k=2 (k=1 earned + this attempt)

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "ok-1"}, SessionID: "sess-ok"},
		Response: rsp,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msg, 2)
	m.dispatchMu.Unlock()

	// Dispatched immediately (capacity free): the debt folded into
	// StarveClock has no further effect, and none remains on the session.
	assert.Equal(t, time.Duration(0), m.errScheduler.TakeQueueWaitCarry("sess-ok"),
		"enqueuing consumed the session's debt into the request's StarveClock")
	assert.Equal(t, 1, m.ErrInFlightCount())
	select {
	case out := <-rsp:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "got %v", out)
	default:
		t.Fatal("request must dispatch")
	}
}

// TestEvictedRetryResumesWaitWithoutInstantBoost: eviction debt resumes the
// starvation clock but grants NO instant boost. A victim evicted after 100s
// (below the 240s floor) retries with 100s of carried wait — still 140s
// short of the floor — so it must NOT outrank a higher-priority competitor.
// The debt only feeds the existing 240s floor, nothing else.
func TestEvictedRetryResumesWaitWithoutInstantBoost(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.pendingQueue = NewPendingQueue(2)

	// Queue full: victim (step 2, queued 100s ago) + competitor (step 8).
	victimRsp := make(chan interface{}, 1)
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "ev-2", SessionID: "sess-ev", CurStep: 2,
		Msg:         &ControlMessage{Request: &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "ev-2"}}, Response: victimRsp},
		EnqueueTime: time.Now().Add(-100 * time.Second),
	}))
	compRsp := make(chan interface{}, 1)
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "comp-8", SessionID: "sess-comp", CurStep: 8,
		Msg: &ControlMessage{Request: &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "comp-8"}}, Response: compRsp},
	}))

	// A step-3 arrival evicts the lowest-step victim (2 < 8), records its
	// 100s wait as debt, and takes the free slot itself.
	bigRsp := make(chan interface{}, 1)
	bigMsg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "big-3"}, SessionID: "sess-big"},
		Response: bigRsp,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(bigMsg, 3)
	m.dispatchMu.Unlock()
	select {
	case out := <-victimRsp:
		assert.Equal(t, ErrQueueFull, out)
	default:
		t.Fatal("victim must be answered")
	}
	select {
	case out := <-bigRsp:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "evictor dispatches into the free slot, got %v", out)
	default:
	}

	// The evicted session retries (k rolled back to 1, this attempt books
	// k=2 → MEJ -2) while the slot is busy: 100s of debt resume its clock.
	m.errScheduler.OnRequest("sess-ev", "model", "p") // k=2 after rollback
	retryRsp := make(chan interface{}, 1)
	retryMsg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "ev-3"}, SessionID: "sess-ev"},
		Response: retryRsp,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(retryMsg, 2)
	m.dispatchMu.Unlock()

	// The slot frees: competitor (-8) must beat the retry (-2, NOT boosted —
	// 100s carried wait is below the 240s floor).
	m.capacityProvider.OnCompleted()
	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	select {
	case out := <-compRsp:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "competitor must win the slot, got %v", out)
	default:
		t.Fatal("competitor must take the freed slot")
	}
	select {
	case <-retryRsp:
		t.Fatal("evicted retry must NOT be instantly boosted below the 240s floor")
	default:
	}
	assert.Equal(t, 1, m.pendingQueue.Size(), "the retry stays queued with its resumed clock")
}

// TestInflightCancelCarriesQueueWait: the pw90 death shape — a request
// starves 288s in the queue, gets dispatched, and the client's budget dies
// 12s later (finishReason=cancelled). The retry must re-enter the queue
// already past the boost threshold: carry += the full starvation-clock age
// at dispatch, not zero.
func TestInflightCancelCarriesQueueWait(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	// The session must exist (dispatchErr's OnRequest creates it).
	m.errScheduler.OnRequest("sess-pw90", "model", "p")

	// The doomed attempt: enqueued with a 288s-old starvation clock.
	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request: &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "pw90-1"}, SessionID: "sess-pw90"},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "pw90-1",
		SessionID: "sess-pw90",
		CurStep:   1,
		Msg:       msg,
		// 288s of accumulated wait (inherited carry or plain age —
		// the starvation clock is what dispatch reads).
		StarveClock: time.Now().Add(-288 * time.Second),
	}))
	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
	assert.Equal(t, 1, m.ErrInFlightCount(), "the starved request takes the slot")

	// In-flight abort: stream cancelled, nothing the client saw.
	m.ErrOnResponse("sess-pw90", "pw90-1", "http://ins-1", "", "cancelled")

	// The retry re-enters with the full wait re-credited: the session's
	// carry must be >= 288s (minus epsilon for the dispatch round-trip).
	m.dispatchMu.Lock()
	carry := m.errScheduler.TakeQueueWaitCarry("sess-pw90")
	m.dispatchMu.Unlock()
	assert.GreaterOrEqual(t, carry, 280*time.Second,
		"in-flight cancel must re-credit the full queue wait (288s), not zero")

	// A fresh retry enqueuing with that carry is boosted immediately and
	// wins the freed slot against a younger competitor.
	rspR := make(chan interface{}, 1)
	msgR := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "pw90-2"}, SessionID: "sess-pw90"},
		Response: rspR,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msgR, 1)
	m.dispatchMu.Unlock()

	rspC := make(chan interface{}, 1)
	msgC := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "fresh-9"}, SessionID: "sess-fresh"},
		Response: rspC,
	}
	m.dispatchMu.Lock()
	m.dispatchWithQueue(msgC, 9) // better base priority (MEJ -9 < -1)
	m.dispatchMu.Unlock()

	// Free the slot: the retry (carried 288s) must beat the fresh -9.
	m.dispatchMu.Lock()
	m.capacityProvider.OnCompleted()
	m.dispatchMu.Unlock()

	select {
	case out := <-rspR:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "the retry must take the freed slot, got %v", out)
	default:
		t.Fatal("retry lost to a fresher request — in-flight cancel did not carry the queue wait")
	}
	select {
	case <-rspC:
		t.Fatal("fresh request must not be dispatched before the carried retry")
	default:
	}
}

// TestNormalCompletionDoesNotCarry: a stream that completes normally
// (finishReason=stop) must NOT leave wait credit on the session — the next
// step starts with a clean starvation clock.
func TestNormalCompletionDoesNotCarry(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	m.errScheduler.OnRequest("sess-ok", "model", "p")
	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "ok-1"}, SessionID: "sess-ok"},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "ok-1", SessionID: "sess-ok", CurStep: 1, Msg: msg,
		StarveClock: time.Now().Add(-288 * time.Second), // also starved 288s
	}))
	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	m.ErrOnResponse("sess-ok", "ok-1", "http://ins-1", "", "stop")

	m.dispatchMu.Lock()
	carry := m.errScheduler.TakeQueueWaitCarry("sess-ok")
	m.dispatchMu.Unlock()
	assert.Equal(t, time.Duration(0), carry,
		"normal completion must not leak wait credit (OnFinish path drops the entry)")
}

// TestUndoDispatchedCarriesQueueWait: the Deliver-abandoned path (requester
// gave up between the dispatch decision and delivery) must re-credit the
// entry's queue wait too.
func TestUndoDispatchedCarriesQueueWait(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	m.errScheduler.OnRequest("sess-undo", "model", "p")

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "undo-1"}, SessionID: "sess-undo"},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "undo-1", SessionID: "sess-undo", CurStep: 1, Msg: msg,
		StarveClock: time.Now().Add(-250 * time.Second),
	}))
	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	// Requester abandons (Deliver returns false path).
	m.dispatchMu.Lock()
	m.undoDispatched(&ScheduleRequestMsg{
		Request:  &LlmRequest{ReqId: "undo-1"},
		SessionID: "sess-undo",
	}, &ScheduleResult{PrefillUrl: "http://ins-1"}, false)
	m.dispatchMu.Unlock()

	m.dispatchMu.Lock()
	carry := m.errScheduler.TakeQueueWaitCarry("sess-undo")
	m.dispatchMu.Unlock()
	assert.GreaterOrEqual(t, carry, 240*time.Second,
		"undoDispatched must re-credit the entry's queue wait")
}
