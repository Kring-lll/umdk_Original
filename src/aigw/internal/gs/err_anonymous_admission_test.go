/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 * Description: anonymous requests must never be rejected by the full-queue
 * admission rule — their boost-tier queue order requires matching
 * admission semantics.
 */

package gs

import (
	"fmt"
	"testing"

	"huawei.com/aigw/internal/stats"

	"github.com/stretchr/testify/assert"
)

// TestAnonymousNeverRejectedOnFullQueue: with the queue full of session
// requests and capacity exhausted, an anonymous request must not be
// rejected — it evicts the least-progressed victim and enters at the boost
// tier, exactly like its queue-order rank promises.
func TestAnonymousNeverRejectedOnFullQueue(t *testing.T) {
	m := newErrTestGSM(t) // queue mode, queueSize=50, batchSize=1
	m.scheduler = starvationTestLB{}
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	// Exhaust capacity so nothing drains from the queue.
	m.capacityProvider.OnDispatched()
	assert.False(t, m.capacityProvider.HasCapacity())

	// Fill the queue with session requests (steps 1..50).
	victimMsgs := make([]*ControlMessage, 0, 50)
	for i := 0; i < 50; i++ {
		msg := &ControlMessage{
			Request: &ScheduleRequestMsg{
				Request:   &LlmRequest{ReqId: fmt.Sprintf("r-%d", i)},
				SessionID: fmt.Sprintf("sess-%d", i),
			},
			Response: make(chan interface{}, 1),
		}
		victimMsgs = append(victimMsgs, msg)
		m.dispatchWithQueue(msg, i+1)
	}
	assert.True(t, m.pendingQueue.IsFull())

	// Anonymous request: dispatchErr computes curStep=1 for it (OnRequest of
	// an empty session). Pre-fix this was always rejected (1 <= any victim
	// step); post-fix it must enter by evicting the lowest-step victim.
	anonMsg := &ControlMessage{
		Request: &ScheduleRequestMsg{
			Request: &LlmRequest{ReqId: "anon-1"},
		},
		Response: make(chan interface{}, 1),
	}
	m.dispatchWithQueue(anonMsg, 1)

	select {
	case out := <-anonMsg.Response:
		t.Fatalf("anonymous request must not be rejected on a full queue, got: %v", out)
	default:
	}

	// The queue stayed full (anonymous in, one victim out) and the evicted
	// victim — the least-progressed session, sess-0 — got ErrQueueFull.
	assert.Equal(t, 50, m.pendingQueue.Size())
	select {
	case out := <-victimMsgs[0].Response:
		assert.Equal(t, ErrQueueFull, out, "the least-progressed victim must be evicted")
	default:
		t.Fatal("the victim must have received ErrQueueFull")
	}

	// Session-bound newcomers at the same step are still rejected (the
	// progress-protection rule keeps protecting real sessions).
	sessionMsg := &ControlMessage{
		Request: &ScheduleRequestMsg{
			Request:   &LlmRequest{ReqId: "r-new"},
			SessionID: "sess-new",
		},
		Response: make(chan interface{}, 1),
	}
	m.dispatchWithQueue(sessionMsg, 1)
	select {
	case out := <-sessionMsg.Response:
		assert.Equal(t, ErrQueueFull, out, "session newcomer at the lowest step must still be rejected")
	default:
		t.Fatal("session newcomer must be rejected on a full queue")
	}
}
