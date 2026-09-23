/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for internal reqID uniqueness on collisions.
 * Create: 2026-09-04
 */

package server

import (
	"context"
	"testing"

	"huawei.com/aigw/internal/core"
)

// TestReqIDCollisionInternalized: regression test.
// Multiple upstreams generate request UUIDs independently and may collide.
// A colliding UUID must NOT cause (a) the second request to be falsely
// rejected by the dedup check, nor (b) an errInFlight registry overwrite
// (which leaks capacity and loses one session's accounting). The gateway
// must internalize the caller-supplied UUID so registry keys are unique.
func TestReqIDCollisionInternalized(t *testing.T) {
	srv, g := setupErrForwardServer(t, "uuid-collision-model")
	_ = srv

	const dupUUID = "upstream-generated-id-42"

	// Two requests with the SAME caller-supplied UUID, both in-flight
	// simultaneously (worst case: concurrent upstreams).
	out1, err1 := srv.manager.GetSuggestion(&core.GetSuggestionIn{
		UUID:   dupUUID,
		Model:  "uuid-collision-model",
		Prompt: "first concurrent request",
		Ctx:    context.Background(),
	})
	out2, err2 := srv.manager.GetSuggestion(&core.GetSuggestionIn{
		UUID:   dupUUID,
		Model:  "uuid-collision-model",
		Prompt: "second concurrent request",
		Ctx:    context.Background(),
	})

	if err1 != nil {
		t.Fatalf("first request with UUID %q failed: %v", dupUUID, err1)
	}
	if err2 != nil {
		t.Fatalf("second request with colliding UUID %q was rejected or overwrote the first: %v", dupUUID, err2)
	}
	if out1.TargetPrefillUrl == "" || out2.TargetPrefillUrl == "" {
		t.Fatalf("both requests should dispatch to the unreachable instance, got %q / %q",
			out1.TargetPrefillUrl, out2.TargetPrefillUrl)
	}

	if n := g.ErrInFlightCount(); n != 2 {
		t.Fatalf("colliding UUIDs must produce 2 distinct in-flight entries, got %d "+
			"(entry overwritten: capacity leak + lost session accounting)", n)
	}
}

// TestReqIDInternalizedFormat: the internalized ID must keep the original
// UUID as a prefix (log traceability) and be distinct per call.
func TestReqIDInternalizedFormat(t *testing.T) {
	srv, _ := setupErrForwardServer(t, "uuid-format-model")

	in := &core.GetSuggestionIn{
		UUID:   "traceable-prefix",
		Model:  "uuid-format-model",
		Prompt: "x",
		Ctx:    context.Background(),
	}
	if _, err := srv.manager.GetSuggestion(in); err != nil {
		t.Fatalf("request failed: %v", err)
	}

	if in.UUID == "traceable-prefix" {
		t.Fatalf("in.UUID must be rewritten to the internalized form, still raw: %q", in.UUID)
	}
	if len(in.UUID) <= len("traceable-prefix#") {
		t.Fatalf("internalized ID should be '<original>#<seq>', got %q", in.UUID)
	}
}
