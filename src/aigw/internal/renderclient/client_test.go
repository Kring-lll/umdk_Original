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

// TestWithAuth_Forwards verifies that a non-empty auth value is forwarded
// verbatim (no scheme rewrite) to the render endpoint.
func TestWithAuth_Forwards(t *testing.T) {
	c := &RenderClient{}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req, "Bearer sk-test")
	if got := req.Header.Get("Authorization"); got != "Bearer sk-test" {
		t.Fatalf("expected verbatim 'Bearer sk-test', got %q", got)
	}
}

// TestWithAuth_EmptySkips verifies that when the inbound request carried no
// Authorization (empty value), no header is sent — preserving backward
// compatibility with unauthenticated backends (which ignore the header
// anyway, so sending it is harmless, but we still skip it for cleanliness).
func TestWithAuth_EmptySkips(t *testing.T) {
	c := &RenderClient{}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req, "")
	if got := req.Header.Get("Authorization"); got != "" {
		t.Fatalf("expected no Authorization header for empty auth value, got %q", got)
	}
}

// TestRenderChat_SendsAuth end-to-end: the render client must forward the
// supplied Authorization value on the wire and reach a 200 handler (proving
// the vLLM --api-key check passes without a 401).
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

	c := NewRenderClient(RenderClientConfig{}, "m", srv.URL)
	ids, err := c.RenderChat(context.Background(), "m", []ChatMessage{{Role: "user", Content: "hi"}}, "Bearer sk-test")
	if err != nil {
		t.Fatalf("RenderChat failed: %v", err)
	}
	if len(ids) != 3 {
		t.Fatalf("expected 3 tokens, got %v", ids)
	}
}
