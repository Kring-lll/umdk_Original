/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for review-round-2 residual fixes.
 * Create: 2026-09-04
 */

package gs

import (
	"errors"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

// TestErrOnCancelFailsQueuedSiblingResponses: regression test.
// When a session's in-flight request is cancelled, its queued sibling
// requests must be removed AND their Response channels written a terminal
// error — otherwise their handlers hang until the client ctx timeout.
func TestErrOnCancelFailsQueuedSiblingResponses(t *testing.T) {
	m := newErrTestGSM(t)

	// Session sess-A has an in-flight request x-1 (dispatched) and a queued
	// sibling q-1 (concurrent submission race, e.g. client retry).
	m.dispatchMu.Lock()
	m.errInFlight["x-1"] = errInFlightEntry{
		SessionID:  "sess-A",
		PrefillURL: "http://ins-1",
	}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	siblingRsp := make(chan interface{}, 1)
	siblingMsg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "q-1"}},
		Response: siblingRsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "q-1",
		SessionID: "sess-A",
		Msg:       siblingMsg,
	}))

	// The in-flight request fails (forward failure).
	m.ErrOnCancel("sess-A", "x-1", "http://ins-1", "")

	select {
	case out := <-siblingRsp:
		err, ok := out.(error)
		if !ok || !errors.Is(err, ErrNoInstance) {
			t.Fatalf("queued sibling must receive ErrNoInstance, got %v", out)
		}
	case <-time.After(time.Second):
		t.Fatal("queued sibling response never written — handler hangs until client timeout")
	}

	assert.Equal(t, 0, m.pendingQueue.Size(), "queue must be drained for the cancelled session")
	assert.Equal(t, 0, m.ErrInFlightCount(), "in-flight entry must be released")
}

// TestRefreshGuardDetectsMidBuildMutation verifies the stale-store guard
// input: a data mutation must advance gen so a Refresh that copied older
// data skips its Store. The full A/B interleaving needs concurrent refresh
// callers (only reachable with future online-learning paths), so this test
// checks the mechanism and the post-mutation convergence.
func TestRefreshGuardDetectsMidBuildMutation(t *testing.T) {
	d := NewEmpiricalDist(0)
	d.AddValue("s1", 5)

	d.mu.Lock()
	d.dirty = false
	genAtCopy := d.gen
	d.mu.Unlock()

	d.AddValue("s2", 100)

	d.mu.Lock()
	advanced := d.gen != genAtCopy
	d.mu.Unlock()
	assert.True(t, advanced, "AddValue must advance gen to invalidate in-flight refresh copies")

	d.Refresh()
	assert.Equal(t, 2, d.TotalCount(), "post-mutation refresh must publish both entries")
	assert.Equal(t, float64(0), d.ExpectedRemaining(100))
}
