/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: survival-scan tests for ERR in-flight zombie requests.
 * Create: 2026-09-03
 */

package gs

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

// TestErrInFlightZombieCleanedBySurvivalScan: regression test.
// A dispatched request whose completion callback is lost forever (handler
// panic, unplugged code path) must be force-released by the periodic ERR
// survival scan, mirroring what checkReqSurvival does for the LB's reqSet.
func TestErrInFlightZombieCleanedBySurvivalScan(t *testing.T) {
	m := newErrTestGSM(t)

	// Simulate a dispatch whose callback will never arrive.
	m.dispatchMu.Lock()
	m.errInFlight["zombie-1"] = errInFlightEntry{
		SessionID:    "sess-zombie",
		PrefillURL:   "http://ins-1",
		DispatchTime: time.Now().Add(-2 * time.Hour),
	}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	assert.Equal(t, 1, m.ErrInFlightCount())
	assert.Equal(t, int32(1), m.capacityProvider.InFlight())

	m.dispatchMu.Lock()
	m.checkErrInFlightSurvival()
	m.dispatchMu.Unlock()

	assert.Equal(t, 0, m.ErrInFlightCount(), "zombie in-flight entry must be force-released")
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "capacity must be returned")
}

// TestErrInFlightSurvivalScanKeepsFreshEntries: the scan must not touch
// entries younger than the survival threshold.
func TestErrInFlightSurvivalScanKeepsFreshEntries(t *testing.T) {
	m := newErrTestGSM(t)

	m.dispatchMu.Lock()
	m.errInFlight["live-1"] = errInFlightEntry{
		SessionID:    "sess-live",
		PrefillURL:   "http://ins-1",
		DispatchTime: time.Now(),
	}
	m.capacityProvider.OnDispatched()
	m.checkErrInFlightSurvival()
	m.dispatchMu.Unlock()

	assert.Equal(t, 1, m.ErrInFlightCount(), "fresh in-flight entry must be kept")
	assert.Equal(t, int32(1), m.capacityProvider.InFlight())
}

// TestErrInFlightSurvivalScanReleasesOnlyTheLeakedEntry: a zombie entry aged
// past the threshold belongs to a session that may still be ALIVE and
// mid-task (v2wl2: a leaked step-3 entry sat 3600s while its session reached
// step 10; the old session-wide collateral — OnCancel + RemoveBySession —
// killed the queued step 10 with a bogus "no instance" 503). The scan must
// release ONLY the leaked entry: the queued request stays queued, the
// session keeps its step history, and a truly dead session is cleaned up by
// its own idle timer.
func TestErrInFlightSurvivalScanReleasesOnlyTheLeakedEntry(t *testing.T) {
	m := newErrTestGSM(t)

	m.errScheduler.OnRequest("sess-zombie", "test-model", "prompt")
	m.errScheduler.OnRequest("sess-zombie", "test-model", "prompt")

	siblingMsg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "q-1"}},
		Response: make(chan interface{}, 1),
	}
	if err := m.pendingQueue.Add(&PendingRequest{
		ReqID:     "q-1",
		SessionID: "sess-zombie",
		CurStep:   3,
		Msg:       siblingMsg,
	}); err != nil {
		t.Fatalf("failed to enqueue sibling: %v", err)
	}

	m.dispatchMu.Lock()
	m.errInFlight["zombie-1"] = errInFlightEntry{
		SessionID:    "sess-zombie",
		PrefillURL:   "http://ins-1",
		DispatchTime: time.Now().Add(-2 * time.Hour),
	}
	m.capacityProvider.OnDispatched()
	m.checkErrInFlightSurvival()
	m.dispatchMu.Unlock()

	assert.Equal(t, 0, m.ErrInFlightCount())
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "capacity must be returned")
	assert.Equal(t, 1, m.pendingQueue.Size(), "the live session's queued request must stay queued")
	select {
	case out := <-siblingMsg.Response:
		t.Fatalf("queued request must not be answered by the zombie scan, got %v", out)
	default:
	}
	assert.Equal(t, 2, m.errScheduler.SessionIteration("sess-zombie"),
		"the live session keeps its step history")
}
