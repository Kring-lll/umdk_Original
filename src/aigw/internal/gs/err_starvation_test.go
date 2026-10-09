/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for starvation floor and queue-timeout session handling.
 * Create: 2026-09-07
 */

package gs

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/stats"
)

type starvationTestLB struct{}

func (starvationTestLB) schedule(*ScheduleRequestMsg, *ScheduleOptions) *ScheduleResult {
	return &ScheduleResult{ResultType: DispatchRequest, PrefillUrl: "http://ins-1"}
}

type starvationTestMetric struct{}

func (starvationTestMetric) GetInstanceMetrics([]string, *MetricQueryOptions) ([]*InstanceMetric, error) {
	return nil, nil
}
func (starvationTestMetric) AddRequest(*LlmRequest, *InstanceContext) error    { return nil }
func (starvationTestMetric) RemoveRequest(*LlmRequest, *InstanceContext) error { return nil }
func (starvationTestMetric) PredictTokensByEMA(*LlmRequest) int                { return 0 }
func (starvationTestMetric) GetDPAwareMetrics(*MetricQueryOptions, int) ([]*DPAwareMetric, error) {
	return nil, nil
}
func (starvationTestMetric) RangeMetrics([]string, *MetricQueryOptions, func(*InstanceMetric) bool) error {
	return nil
}

// TestQueueBoostRescuesStarvedRequest: regression test (poisson benchmark:
// 10 office tasks starved to the 300s client budget under ERR/MEJ).
// A request queued beyond queueBoostSec must be forced to top priority so
// sustained overload cannot starve it into the client's first-chunk timeout.
func TestQueueBoostRescuesStarvedRequest(t *testing.T) {
	m := newErrTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()
	// All sessions take 80 steps: E[rem|1]=79 (worst), E[rem|10]=70 (better).
	m.errScheduler.globalDist.SeedFromCounts(map[int]int{80: 10})

	// Request A: k=1, worst priority, queued 300s ago (beyond the 240s floor).
	rspA := make(chan interface{}, 1)
	msgA := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "starved-a"}},
		Response: rspA,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:       "starved-a",
		SessionID:   "sess-starved",
		CurStep:     1,
		Msg:         msgA,
		EnqueueTime: time.Now().Add(-300 * time.Second),
	}))

	// Request B: k=10, better base priority, freshly queued.
	rspB := make(chan interface{}, 1)
	msgB := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "fresh-b"}},
		Response: rspB,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "fresh-b",
		SessionID: "sess-fresh",
		CurStep:   10,
		Msg:       msgB,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	// The starved request must win the single slot despite worse base priority.
	select {
	case out := <-rspA:
		s, ok := out.(*SuggestionResultMsg)
		if !ok || s.PrefillUrl == "" {
			t.Fatalf("starved request dispatched with unexpected result: %v", out)
		}
	default:
		t.Fatal("starved request was not dispatched — starvation floor did not fire")
	}
	assert.Equal(t, 1, m.ErrInFlightCount())
	assert.Equal(t, 1, m.pendingQueue.Size(), "fresh request stays queued (single slot)")
	select {
	case <-rspB:
		t.Fatal("fresh request must not be dispatched before the starved one")
	default:
	}
}

// TestQueueTimeoutRollsBackIteration: regression test (poisson benchmark §6.6:
// timed-out tasks retried at k=1,1,1 because OnCancel deleted the session).
// A queued request that times out before dispatch completed no step: k is
// rolled back, the session survives, and the retry re-enters at the same k
// with its earned priority.
func TestQueueTimeoutRollsBackIteration(t *testing.T) {
	m := newErrTestGSM(t)
	m.errScheduler.OnRequest("sess-A", "model", "p") // k=1
	m.errScheduler.OnRequest("sess-A", "model", "p") // k=2

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "r-2"}},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "r-2", SessionID: "sess-A", CurStep: 2, Msg: msg,
	}))

	// The queued request hits the client's wait budget (never dispatched).
	m.ErrOnCancel("sess-A", "r-2", "", "")

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-A"),
		"the failed attempt must not count as a step")
	assert.Equal(t, 2, m.errScheduler.OnRequest("sess-A", "model", "p"),
		"the retry re-enters at the same step number")
	select {
	case out := <-rsp:
		if _, ok := out.(error); !ok {
			t.Fatalf("queued request must receive a terminal error, got %v", out)
		}
	default:
		t.Fatal("queued request response never written")
	}
}

// TestInflightAbortRollsBackAndKeepsSession guards the in-flight branch:
// a mid-request abort (client hung up after dispatch) completes no step the
// client saw, so k rolls back and the session survives for the SDK retry —
// consistent with the queue-timeout semantics. Only the idle timer deletes
// sessions.
func TestInflightAbortRollsBackAndKeepsSession(t *testing.T) {
	m := newErrTestGSM(t)
	m.errScheduler.OnRequest("sess-B", "model", "p") // k=1
	m.errScheduler.OnRequest("sess-B", "model", "p") // k=2 (the aborted attempt)

	m.dispatchMu.Lock()
	m.errInFlight["r-1"] = errInFlightEntry{SessionID: "sess-B", PrefillURL: "http://ins-1"}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	m.ErrOnCancel("sess-B", "r-1", "http://ins-1", "")

	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-B"),
		"in-flight abort rolls k back, the session survives")
	assert.Equal(t, 2, m.errScheduler.OnRequest("sess-B", "model", "p"),
		"the retry re-enters at the same step number")
	assert.Equal(t, 0, m.ErrInFlightCount())
}

// TestClassifyPromptPrefersUserPrompt: classification input is the user-role
// text when supplied (avoids 34.8k system prompts dominating TF-IDF), with
// fallback to the full prompt for legacy callers.
func TestClassifyPromptPrefersUserPrompt(t *testing.T) {
	req := &ScheduleRequestMsg{Request: &LlmRequest{
		Prompt:     "system: You are JiuwenSwarm... (34.8k chars)",
		UserPrompt: "user task text",
	}}
	if got := classifyPrompt(req); got != "user task text" {
		t.Fatalf("UserPrompt must win, got %q", got)
	}
	legacy := &ScheduleRequestMsg{Request: &LlmRequest{Prompt: "full only"}}
	if got := classifyPrompt(legacy); got != "full only" {
		t.Fatalf("legacy fallback to Prompt, got %q", got)
	}
}

// newMejTestGSM returns a GSM whose ERR scheduler runs MEJ, whose priority
// domain is negative (-curStep). The starvation floor sentinel must beat it.
func newMejTestGSM(t *testing.T) *GlobalSchedulerManager {
	t.Helper()
	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "mej",
		QueueSize:          50,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	return m
}

// TestQueueBoostBeatsNegativePriorityDomain: the boost sentinel must rank
// below MEJ's negative priorities. With the sentinel at 0 the starved request
// became the WORST-ranked one under MEJ, inverting the floor into guaranteed
// starvation (fresh k=8 at -8 always outranked starved k=1 boosted to 0).
func TestQueueBoostBeatsNegativePriorityDomain(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	// Request A: k=1 (MEJ priority -1), queued 300s ago (beyond the 240s floor).
	rspA := make(chan interface{}, 1)
	msgA := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "starved-mej-a"}},
		Response: rspA,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:       "starved-mej-a",
		SessionID:   "sess-starved",
		CurStep:     1,
		Msg:         msgA,
		EnqueueTime: time.Now().Add(-300 * time.Second),
	}))

	// Request B: k=8 (MEJ priority -8, nominally better), freshly queued.
	rspB := make(chan interface{}, 1)
	msgB := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "fresh-mej-b"}},
		Response: rspB,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "fresh-mej-b",
		SessionID: "sess-fresh",
		CurStep:   8,
		Msg:       msgB,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	select {
	case out := <-rspA:
		s, ok := out.(*SuggestionResultMsg)
		if !ok || s.PrefillUrl == "" {
			t.Fatalf("starved request dispatched with unexpected result: %v", out)
		}
	default:
		t.Fatal("starved request lost to a fresher one — boost sentinel is not below MEJ's negative domain")
	}
	assert.Equal(t, 1, m.ErrInFlightCount(), "fresh request stays queued (single slot)")
	select {
	case <-rspB:
		t.Fatal("fresh request must not be dispatched before the starved one")
	default:
	}
}

// TestAnonymousRequestsGetBoostTierPriority: anonymous internal calls
// (context compression) synchronously block the parent session and are
// small + latency-sensitive — they rank at the boost tier (MinInt32), not
// at the classifier-derived tier.
func TestAnonymousRequestsGetBoostTierPriority(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	// Anonymous request queued behind a session request with better base
	// priority (MEJ -8 < anonymous's classifier-derived value).
	rspSess := make(chan interface{}, 1)
	msgSess := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "sess-8"}, SessionID: "sess-hi"},
		Response: rspSess,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "sess-8", SessionID: "sess-hi", CurStep: 8, Msg: msgSess,
	}))
	rspAnon := make(chan interface{}, 1)
	msgAnon := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "anon-1"}},
		Response: rspAnon,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID: "anon-1", CurStep: 1, Msg: msgAnon,
	}))

	m.dispatchMu.Lock()
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	select {
	case out := <-rspAnon:
		s, ok := out.(*SuggestionResultMsg)
		assert.True(t, ok && s.PrefillUrl != "", "anonymous must take the slot despite worse base priority")
	default:
		t.Fatal("anonymous request starved below a session request")
	}
}
