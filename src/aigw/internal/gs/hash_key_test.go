/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for hash-key extraction priority —
 * agent_hint.session_id must outrank session_params.session_id so ERR
 * session tracking keys on the SDK-injected identity.
 * Create: 2026-09-22
 */

package gs

import (
	"net/http"
	"testing"
)

func TestExtractHashKeyAgentHintOutranksSessionParams(t *testing.T) {
	body := map[string]interface{}{
		"agent_hint":     map[string]interface{}{"session_id": "agent-hint-sid"},
		"session_params": map[string]interface{}{"session_id": "session-params-sid"},
	}
	got := ExtractHashKey(http.Header{}, body)
	want := "body:agent_hint.session_id:agent-hint-sid"
	if got != want {
		t.Errorf("ExtractHashKey = %q, want %q (agent_hint must outrank session_params)", got, want)
	}
}

func TestExtractHashKeySessionParamsFallback(t *testing.T) {
	body := map[string]interface{}{
		"session_params": map[string]interface{}{"session_id": "session-params-sid"},
	}
	got := ExtractHashKey(http.Header{}, body)
	want := "body:session_params.session_id:session-params-sid"
	if got != want {
		t.Errorf("ExtractHashKey = %q, want %q", got, want)
	}
}

func TestExtractHashKeyHeaderBeatsBody(t *testing.T) {
	h := http.Header{}
	h.Set("x-session-id", "header-sid")
	body := map[string]interface{}{
		"agent_hint": map[string]interface{}{"session_id": "agent-hint-sid"},
	}
	got := ExtractHashKey(h, body)
	want := "header:x-session-id:header-sid"
	if got != want {
		t.Errorf("ExtractHashKey = %q, want %q (headers outrank body keys)", got, want)
	}
}
