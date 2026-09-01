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

// TestReadSSEResponse_SingleLineJSONNoTrailingNewline: render returns single-line
// JSON with no trailing newline (non-SSE path).
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

// TestReadSSEResponse_EmptyBody: empty body returns EOF, not empty data.
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

// TestWithAuth_Forwards: non-empty auth value is forwarded verbatim.
func TestWithAuth_Forwards(t *testing.T) {
	c := &RenderClient{}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req, "Bearer sk-test")
	if got := req.Header.Get("Authorization"); got != "Bearer sk-test" {
		t.Fatalf("expected verbatim 'Bearer sk-test', got %q", got)
	}
}

// TestWithAuth_EmptySkips: empty auth value sends no header.
func TestWithAuth_EmptySkips(t *testing.T) {
	c := &RenderClient{}
	req := httptest.NewRequest("POST", "http://x", nil)
	c.withAuth(req, "")
	if got := req.Header.Get("Authorization"); got != "" {
		t.Fatalf("expected no Authorization header for empty auth value, got %q", got)
	}
}

// TestRenderChat_SendsAuth: forwarded Authorization reaches a 200 handler.
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
