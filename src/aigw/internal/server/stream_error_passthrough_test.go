/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression test for upstream error passthrough on stream requests.
 * Create: 2026-09-07
 */

package server

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"

	"strings"
)

// TestStreamErrorResponseNotMaskedAsSSE: regression test (deployment-found).
// An upstream 4xx on a streaming request must be forwarded with its real
// status code and error body. The old behavior wrote 200 + an empty SSE
// stream: the SSE reader silently dropped the non-SSE error body, the client
// saw a "successful" response with no content, and the real error (e.g. a
// context-length 400 from vLLM) was lost.
func TestStreamErrorResponseNotMaskedAsSSE(t *testing.T) {
	const errBody = `{"error":{"message":"This model's maximum context length is 32769 tokens, however you requested 32900 tokens"}}`
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(errBody))
	}))
	defer backend.Close()

	bu, err := url.Parse(backend.URL)
	if err != nil {
		t.Fatalf("parse backend url: %v", err)
	}
	srv, g := setupErrForwardServerWithInstance(t, "stream-err-model", bu.Hostname(), bu.Port())

	resp := postChatCompletion(t, srv, "stream-err-model",
		`{"model":"stream-err-model","stream":true,`+
			`"messages":[{"role":"user","content":"hello"}]}`)

	if resp.Code != http.StatusBadRequest {
		t.Fatalf("upstream 400 must reach the client, got %d (body: %s)", resp.Code, resp.Body.String())
	}
	if body := resp.Body.String(); body == "" {
		t.Fatal("error body must be forwarded, got empty body (error detail lost)")
	} else if !strings.Contains(body, "maximum context length") {
		t.Fatalf("error body not passed through, got: %s", body)
	}
	if ct := resp.Header().Get("Content-Type"); ct == "text/event-stream" {
		t.Fatal("error response must not be masked as SSE")
	}

	// The dispatch slot must still be released (ErrOnResponse with unknown
	// finishReason releases in-flight without polluting the distribution).
	if n := g.ErrInFlightCount(); n != 0 {
		t.Fatalf("in-flight slot leaked on upstream 4xx: %d", n)
	}
}
