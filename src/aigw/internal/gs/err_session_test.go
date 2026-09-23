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
