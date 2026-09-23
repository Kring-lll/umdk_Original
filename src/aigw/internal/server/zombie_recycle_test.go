/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for the v2wl2 zombie-recycle false kill.
 * Leaked in-flight entries must not take live queued requests down with
 * them, and a handler writing to an abandoned (unclosed, unread) client
 * connection must time out and release its slot instead of hanging forever.
 * Create: 2026-09-09
 */

package server

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/core"
	"huawei.com/aigw/internal/gs"
	"huawei.com/aigw/internal/proxy"
)

// setupStreamingErrServer builds an ERR aigw server (real TCP, via
// httptest.NewServer) whose single instance is a controllable SSE mock.
func setupStreamingErrServer(t *testing.T, model string, instanceHandler http.HandlerFunc) (*httptest.Server, *gs.GlobalSchedulerManager, *HttpServer) {
	t.Helper()

	instance := httptest.NewServer(instanceHandler)
	t.Cleanup(instance.Close)

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

	host, portStr, _ := net.SplitHostPort(strings.TrimPrefix(instance.URL, "http://"))
	rsp := make(chan interface{}, 1)
	regMsg := &gs.ControlMessage{
		Request: &gs.RegisterInstanceMsg{
			Name: "mock-ins", Model: model, IP: host, Port: portStr, Role: "mixed",
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

	mgr := core.NewAigwManagerForTest(&base.AigwConfig{}, nil,
		map[string]*gs.GlobalSchedulerManager{model: g})
	pm := proxy.NewProxyManager(context.Background(), &proxy.ProxyConfig{
		Timeout:  30 * time.Second,
		MaxRetry: 0,
	})
	srv := NewHttpServer(mgr, "127.0.0.1", "0", pm)
	ts := httptest.NewServer(http.HandlerFunc(srv.forwardChatCompletions))
	t.Cleanup(ts.Close)
	return ts, g, srv
}

// TestStalledClientWriteTimesOutAndReleases: v2wl2 leak class. The client
// abandons the stream without closing the connection (async-stream timeout,
// no aclose) and stops reading; the mock instance keeps producing data. The
// handler must NOT hang forever on the blocked client write — the per-write
// deadline converts it into a cancellation and the in-flight slot is
// released.
func TestStalledClientWriteTimesOutAndReleases(t *testing.T) {
	ts, g, srv := setupStreamingErrServer(t, "stall-model", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		flusher := w.(http.Flusher)
		// Enough data to overflow any kernel buffer on the client side.
		payload := strings.Repeat("x", 32*1024)
		for i := 0; i < 512; i++ {
			fmt.Fprintf(w, "data: %s\n\n", payload)
			flusher.Flush()
		}
		fmt.Fprintf(w, "data: [DONE]\n\n")
		flusher.Flush()
	})

	srv.proxyMgr.SetChunkIntervalTimeoutForTest(300 * time.Millisecond)

	// A raw client that sends the request and then never reads, keeping the
	// connection open — the abandoned httpx connection.
	addr := strings.TrimPrefix(ts.URL, "http://")
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	tcpConn, ok := conn.(*net.TCPConn)
	if !ok {
		t.Fatalf("expected *net.TCPConn, got %T", conn)
	}
	if err := tcpConn.SetReadBuffer(2048); err != nil {
		t.Fatalf("set read buffer: %v", err)
	}
	body := `{"model":"stall-model","stream":true,"messages":[{"role":"user","content":"hi"}],"agent_hint":{"session_id":"sess-stall"}}`
	fmt.Fprintf(conn, "POST /aigw/v1/openai/chat/completions HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
		addr, len(body), body)

	// The request must dispatch first (in-flight = 1), then be released by
	// the write timeout — otherwise the test proves nothing.
	waitFor := func(want int, what string) {
		deadline := time.Now().Add(10 * time.Second)
		for time.Now().Before(deadline) {
			if g.ErrInFlightCount() == want {
				return
			}
			time.Sleep(50 * time.Millisecond)
		}
		assert.Equal(t, want, g.ErrInFlightCount(), what)
	}
	waitFor(1, "the request must dispatch to the mock instance first")
	waitFor(0, "the in-flight slot must be released after the client write times out — a leaked slot starves real requests and (v2wl2) arms the zombie recycle")
}

// TestDeadUpstreamStreamReleasesSlot: the v2wl2-cancel-storm deadlock shape
// on the read side — the upstream accepts the request, sends response
// headers, then goes silent forever (engine crash). The stream idle
// watchdog must abort the read and release the in-flight slot within the
// idle timeout instead of hanging until the zombie scan.
func TestDeadUpstreamStreamReleasesSlot(t *testing.T) {
	ts, g, srv := setupStreamingErrServer(t, "dead-upstream-model", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		w.(http.Flusher).Flush()
		// headers sent, then silence forever
		<-r.Context().Done()
	})

	// Shorten the idle watchdog through the server's proxy manager (1s:
	// wide enough for the poll to observe the in-flight window).
	srv.proxyMgr.SetFirstChunkTimeoutForTest(1 * time.Second)

	body := `{"model":"dead-upstream-model","stream":true,"messages":[{"role":"user","content":"hi"}],"agent_hint":{"session_id":"sess-dead-up"}}`
	req, err := http.NewRequest(http.MethodPost, ts.URL+"/aigw/v1/openai/chat/completions", strings.NewReader(body))
	if err != nil {
		t.Fatalf("new request: %v", err)
	}
	req.Header.Set("Content-Type", "application/json")
	// Fire asynchronously: the aigw handler writes no bytes until the
	// upstream speaks, so a synchronous Do would block until the whole
	// dispatch→idle-abort→release chain has already finished.
	respCh := make(chan *http.Response, 1)
	go func() {
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			t.Errorf("post: %v", err)
			return
		}
		respCh <- resp
	}()
	defer func() {
		select {
		case resp := <-respCh:
			resp.Body.Close()
		default:
		}
	}()

	// The dispatch→forward→idle-abort→release chain can complete inside
	// one poll interval, so track the peak instead of waiting for 1 first.
	peak := 0
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if n := g.ErrInFlightCount(); n > peak {
			peak = n
		}
		if peak >= 1 && g.ErrInFlightCount() == 0 {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	assert.GreaterOrEqual(t, peak, 1, "the request must have dispatched")
	assert.Equal(t, 0, g.ErrInFlightCount(),
		"the slot must be released after the upstream goes silent — the cancel-storm deadlock freed only at the zombie scan")
}
