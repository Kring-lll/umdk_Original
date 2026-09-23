/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression test for suggestion-endpoint capacity release.
 * Create: 2026-09-04
 */

package server

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/core"
)

// TestSuggestionEndpointReleasesInFlight: regression test.
// /get-suggestion is an advisory endpoint: it returns a target URL but never
// forwards the request, so no proxy completion callback will ever fire.
// Its dispatch must not take an in-flight slot at all (the caller forwards
// the request itself — aigw never observes completion).
func TestSuggestionEndpointReleasesInFlight(t *testing.T) {
	srv, g := setupErrForwardServer(t, "suggestion-model")

	serverHandler.cfgMgr = core.NewAigwConfigManager()
	if err := serverHandler.cfgMgr.LoadConfig(validConfigPath); err != nil {
		t.Fatalf("failed to load test config: %v", err)
	}

	body := `{"uuid":"query-1","model":"suggestion-model",` +
		`"messages":[{"role":"user","content":"hello"}]}`
	req := httptest.NewRequest(http.MethodPost, "/aigw/v1/openai/get-suggestion",
		bytes.NewReader([]byte(body)))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	srv.scheduleForOpenAi(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200 with a suggestion, got %d (body: %s)",
			rr.Code, rr.Body.String())
	}
	var out base.GetSuggestionOut
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("response not parseable: %v", err)
	}
	if out.TargetPrefillUrl == "" {
		t.Fatalf("expected a target instance in the suggestion")
	}

	if n := g.ErrInFlightCount(); n != 0 {
		t.Fatalf("advisory get-suggestion must not take in-flight slots at all, got %d", n)
	}
}
