/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Render client SSE/JSON body reader tests.
 */

package renderclient

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// TestReadSSEResponse_SingleLineJSONNoTrailingNewline pins the non-SSE response
// path. vLLM's /v1/chat/completions/render returns a single JSON object with no
// trailing newline (and no SSE "data:" framing) when stream mode is not used.
// bufio.Reader.ReadBytes('\n') on such a body returns (data, io.EOF)
// simultaneously; the previous implementation checked the error before writing
// the partial line to the buffer, so buf.Len()==0 on EOF and the function
// returned "failed to read response body: EOF", dropping the JSON and breaking
// every non-SSE render call.
func TestReadSSEResponse_SingleLineJSONNoTrailingNewline(t *testing.T) {
	body := []byte(`{"request_id":"abc","token_ids":[151644,8948,198]}`)
	c := &RenderClient{}

	out, err := c.readSSEResponse(bytes.NewReader(body))
	if err != nil {
		t.Fatalf("readSSEResponse returned error for single-line JSON: %v", err)
	}
	if !bytes.Contains(out, body) {
		t.Fatalf("expected output to contain the JSON body, got: %q", out)
	}
}

// TestReadSSEResponse_SSEDataLines pins the original SSE path still works.
func TestReadSSEResponse_SSEDataLines(t *testing.T) {
	body := []byte("data: {\"token_ids\":[1,2,3]}\n\n")
	c := &RenderClient{}

	out, err := c.readSSEResponse(bytes.NewReader(body))
	if err != nil {
		t.Fatalf("readSSEResponse returned error for SSE body: %v", err)
	}
	if !strings.Contains(string(out), "data: ") {
		t.Fatalf("expected output to retain SSE data line, got: %q", out)
	}
}

// TestReadSSEResponse_EmptyBody ensures a truly empty body still returns EOF
// (not silently returning empty data that masks a real error).
func TestReadSSEResponse_EmptyBody(t *testing.T) {
	c := &RenderClient{}
	_, err := c.readSSEResponse(bytes.NewReader(nil))
	if err == nil {
		t.Fatalf("expected error for empty body, got nil")
	}
	if err != io.EOF {
		t.Fatalf("expected io.EOF for empty body, got %v", err)
	}
}

// TestWithAuth_NoKey pins backward compatibility: with no APIKey configured the
// client must not touch the Authorization header, so unauthenticated vLLM
// backends keep working.
func TestWithAuth_NoKey(t *testing.T) {
	c := &RenderClient{}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req)
	if got := req.Header.Get("Authorization"); got != "" {
		t.Fatalf("expected no Authorization header, got %q", got)
	}
}

// TestWithAuth_BearerDefault verifies that a configured key is sent as
// "Authorization: Bearer <key>" by default (vLLM --api-key convention).
func TestWithAuth_BearerDefault(t *testing.T) {
	c := &RenderClient{config: RenderClientConfig{APIKey: "sk-test"}}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req)
	if got := req.Header.Get("Authorization"); got != "Bearer sk-test" {
		t.Fatalf("expected 'Bearer sk-test', got %q", got)
	}
}

// TestWithAuth_CustomHeader verifies a custom header name + prefix, mirroring
// apipool.Adapter.InjectAuth (e.g. providers that use "api-key: <key>").
func TestWithAuth_CustomHeader(t *testing.T) {
	c := &RenderClient{config: RenderClientConfig{
		APIKey:           "sk-test",
		AuthHeaderName:   "api-key",
		AuthHeaderPrefix: "",
	}}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req)
	if got := req.Header.Get("api-key"); got != "sk-test" {
		t.Fatalf("expected api-key header 'sk-test', got %q", got)
	}
	if got := req.Header.Get("Authorization"); got != "" {
		t.Fatalf("expected no Authorization header when custom header set, got %q", got)
	}
}

// TestRenderChat_SendsAuth end-to-end: a render client with an API key must
// reach a 200 handler without 401, proving the key is sent on the wire.
func TestRenderChat_SendsAuth(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer sk-test" {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"token_ids":[1,2,3]}`))
	}))
	defer srv.Close()

	c := NewRenderClient(RenderClientConfig{APIKey: "sk-test"}, "m", srv.URL)
	ids, err := c.RenderChat(context.Background(), "m", []ChatMessage{{Role: "user", Content: "hi"}})
	if err != nil {
		t.Fatalf("RenderChat failed: %v", err)
	}
	if len(ids) != 3 {
		t.Fatalf("expected 3 tokens, got %v", ids)
	}
}
