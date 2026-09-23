/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for ERR in-flight slot release on forward failure.
 * Create: 2026-09-03
 */

package server

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/core"
	"huawei.com/aigw/internal/gs"
	"huawei.com/aigw/internal/proxy"
)

// setupErrForwardServer builds a real end-to-end stack: GSM with ERR queue
// mode, one unreachable registered instance, real proxy manager, HTTP handler.
// Requests dispatch successfully (in-flight slot taken) but always fail at
// proxy forwarding.
func setupErrForwardServer(t *testing.T, model string) (*HttpServer, *gs.GlobalSchedulerManager) {
	return setupErrForwardServerWithInstance(t, model, "127.0.0.1", "1")
}

func setupErrForwardServerWithInstance(t *testing.T, model, ip, port string) (*HttpServer, *gs.GlobalSchedulerManager) {
	t.Helper()

	gsCfg := &base.GlobalSchedulerConfig{
		Model:        model,
		DeployPolicy: "mixed",
		LoadBalancer: base.LoadBalancerConfig{Mixed: "roundRobin", BatchSize: 8},
		ErrScheduler: base.ErrSchedulerConfig{
			ErrSchedulerType:  "mej",
			QueueSize:         10,
			SessionTimeoutSec: 660,
			TaskIdleTimeoutSec: 60,
		},
		SkipInstanceConnection: true,
	}
	g, err := gs.NewGlobalSchedulerManager(context.Background(), gsCfg,
		gs.WithRuntimeMode(base.ServiceMode))
	if err != nil {
		t.Fatalf("failed to create GSM: %v", err)
	}
	if err := g.Start(); err != nil {
		t.Fatalf("failed to start GSM: %v", err)
	}
	t.Cleanup(g.Stop)

	// Register an unreachable instance (port 1 — connection refused).
	rsp := make(chan interface{}, 1)
	regMsg := &gs.ControlMessage{
		Request: &gs.RegisterInstanceMsg{
			Name:  "ins-" + port,
			Model: model,
			IP:    ip,
			Port:  port,
			Role:  "mixed",
		},
		Response: rsp,
	}
	g.PutControlMessage(regMsg)
	select {
	case out := <-rsp:
		if err, ok := out.(error); ok {
			t.Fatalf("instance registration failed: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("instance registration timed out")
	}
	if g.GetInsNum() != 1 {
		t.Fatalf("expected 1 registered instance, got %d", g.GetInsNum())
	}

	mgr := core.NewAigwManagerForTest(&base.AigwConfig{}, nil,
		map[string]*gs.GlobalSchedulerManager{model: g})
	pm := proxy.NewProxyManager(context.Background(), &proxy.ProxyConfig{
		Timeout:  5 * time.Second,
		MaxRetry: 0,
	})
	return NewHttpServer(mgr, "127.0.0.1", "0", pm), g
}

func postChatCompletion(t *testing.T, srv *HttpServer, model string, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, "/aigw/v1/openai/chat/completions",
		bytes.NewReader([]byte(body)))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	srv.forwardChatCompletions(rr, req)
	return rr
}

// TestAnonymousForwardFailureReleasesInFlight: regression test.
// An anonymous request (no agent_hint.session_id) that dispatches
// successfully must release its in-flight slot when proxy forwarding fails.
// The old guard `if sessionID != ""` skipped ErrOnCancel for anonymous
// requests, permanently leaking capacity.
func TestAnonymousForwardFailureReleasesInFlight(t *testing.T) {
	srv, g := setupErrForwardServer(t, "anon-leak-model")

	resp := postChatCompletion(t, srv, "anon-leak-model",
		`{"model":"anon-leak-model","stream":false,"messages":[{"role":"user","content":"hi"}]}`)

	if resp.Code != http.StatusBadGateway {
		t.Fatalf("expected 502 Bad Gateway from unreachable instance, got %d (body: %s)",
			resp.Code, resp.Body.String())
	}

	if n := g.ErrInFlightCount(); n != 0 {
		t.Fatalf("anonymous forward-failure leaked %d in-flight slot(s); "+
			"ErrOnCancel was skipped because sessionID is empty", n)
	}
}

// TestSessionForwardFailureReleasesInFlight: control case — a request WITH a
// session_id takes and releases the same slot, proving the harness observes
// the in-flight lifecycle correctly.
func TestSessionForwardFailureReleasesInFlight(t *testing.T) {
	srv, g := setupErrForwardServer(t, "sess-leak-model")

	resp := postChatCompletion(t, srv, "sess-leak-model",
		`{"model":"sess-leak-model","stream":false,`+
			`"agent_hint":{"session_id":"sess-1"},`+
			`"messages":[{"role":"user","content":"hi"}]}`)

	if resp.Code != http.StatusBadGateway {
		t.Fatalf("expected 502 Bad Gateway, got %d (body: %s)", resp.Code, resp.Body.String())
	}

	if n := g.ErrInFlightCount(); n != 0 {
		t.Fatalf("session forward-failure leaked %d in-flight slot(s)", n)
	}
}
