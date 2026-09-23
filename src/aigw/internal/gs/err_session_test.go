/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: SessionTracker test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
)

func TestSessionTrackerOnRequest(t *testing.T) {
	tracker := NewSessionTracker(time.Second, time.Second)

	k1 := tracker.OnRequest("session-1", "model-1")
	if k1 != 1 {
		t.Errorf("First OnRequest k = %d, want 1", k1)
	}

	k2 := tracker.OnRequest("session-1", "model-1")
	if k2 != 2 {
		t.Errorf("Second OnRequest k = %d, want 2", k2)
	}

	k3 := tracker.OnRequest("session-2", "model-1")
	if k3 != 1 {
		t.Errorf("First OnRequest for new session k = %d, want 1", k3)
	}
}

func TestSessionTrackerOnRequestEmptySessionID(t *testing.T) {
	tracker := NewSessionTracker(time.Second, time.Second)
	k := tracker.OnRequest("", "model-1")
	if k != 1 {
		t.Errorf("OnRequest with empty sessionID k = %d, want 1", k)
	}
}

func TestSessionTrackerOnFinish(t *testing.T) {
	tracker := NewSessionTracker(time.Second, time.Second)

	// Finish without prior OnRequest
	if steps := tracker.OnFinish("unknown"); steps != 0 {
		t.Errorf("OnFinish for unknown session = %d, want 0", steps)
	}

	// Normal flow: 3 iterations then finish
	tracker.OnRequest("session-1", "model-1")
	tracker.OnRequest("session-1", "model-1")
	tracker.OnRequest("session-1", "model-1")
	if steps := tracker.OnFinish("session-1"); steps != 3 {
		t.Errorf("OnFinish steps = %d, want 3", steps)
	}
}

func TestSessionTrackerOnCancel(t *testing.T) {
	tracker := NewSessionTracker(time.Second, time.Second)
	tracker.OnRequest("session-1", "model-1")
	tracker.OnCancel("session-1")

	// After cancel, OnFinish should return 0
	if steps := tracker.OnFinish("session-1"); steps != 0 {
		t.Errorf("OnFinish after cancel = %d, want 0", steps)
	}
}

func TestSessionTrackerCategory(t *testing.T) {
	tracker := NewSessionTracker(time.Second, time.Second)
	tracker.OnRequest("session-1", "model-1")

	tracker.SetCategory("session-1", "cluster_0")
	if cat := tracker.GetCategory("session-1"); cat != "cluster_0" {
		t.Errorf("GetCategory = %q, want 'cluster_0'", cat)
	}

	if cat := tracker.GetCategory("unknown"); cat != "" {
		t.Errorf("GetCategory for unknown session = %q, want ''", cat)
	}
}

func TestSessionTrackerIdleTimeout(t *testing.T) {
	tracker := NewSessionTracker(50*time.Millisecond, 50*time.Millisecond)
	tracker.OnRequest("session-1", "model-1")
	tracker.OnFinish("session-1")

	// Wait for idle timer to fire
	time.Sleep(100 * time.Millisecond)

	// Session should be cleaned up
	if steps := tracker.OnFinish("session-1"); steps != 0 {
		t.Errorf("OnFinish after idle timeout = %d, want 0", steps)
	}
}

// TestIdleTimerRaceActiveSessionSurvives: an idle timer firing concurrently
// with OnRequest must not delete the session — cleanupIdleSession re-checks
// LastActiveTime under the same lock OnRequest refreshes, so a request that
// landed inside the timeout window keeps the session alive.
func TestIdleTimerRaceActiveSessionSurvives(t *testing.T) {
	tr := NewSessionTracker(50*time.Millisecond, 660*time.Second)

	sid := "race-session"
	k := tr.OnRequest(sid, "model")
	assert.Equal(t, 1, k)

	// Simulate the stale-firing window: timer callback runs AFTER a fresh
	// OnRequest refreshed LastActiveTime (Stop() raced and returned false).
	// cleanupIdleSession takes its own lock, so refresh first, then fire.
	tr.mu.Lock()
	s := tr.sessions[sid]
	s.LastActiveTime = time.Now()
	tr.mu.Unlock()
	tr.cleanupIdleSession(sid, 50*time.Millisecond, false)

	assert.Equal(t, 2, tr.OnRequest(sid, "model"),
		"session must survive: fresh activity is inside the timeout window")

	// Control: a session idle beyond the timeout IS cleaned.
	tr.mu.Lock()
	s = tr.sessions[sid]
	s.LastActiveTime = time.Now().Add(-time.Second)
	tr.mu.Unlock()
	tr.cleanupIdleSession(sid, 50*time.Millisecond, false)
	assert.Equal(t, 1, tr.OnRequest(sid, "model"),
		"idle-beyond-timeout session is cleaned and restarts at k=1")
}
