/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: integration tests driving prefixCacheLB.schedule() end to end.
 */

package gs

import (
	"context"
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"huawei.com/aigw/internal/kvevents"
	"huawei.com/aigw/internal/prefixcache"
	"huawei.com/aigw/internal/renderclient"
)

// mockPrefixCacheMgr is a stub PrefixCacheManager for schedule-level tests.
// It serves a pre-seeded url->instanceName map and a pre-seeded matched map,
// without touching render/tokenize/kvevents machinery. renderClient is left
// nil on the LB so schedule() takes the text-fallback (MatchPrefixByText) path.
type mockPrefixCacheMgr struct {
	urlToName map[string]string // url -> instance name
	matched   map[string]int    // instance name -> match percent
	mu        sync.Mutex
	addCalls  int // count of AddPrefix invocations (must stay 0 after fix #2)
}

func (m *mockPrefixCacheMgr) GetInstanceNameByUrl(url string) (string, bool) {
	name, ok := m.urlToName[url]
	return name, ok
}

func (m *mockPrefixCacheMgr) MatchPrefix(_ string, _ int64, _ []int64, _ []string) (map[string]int, int, error) {
	return m.matched, 0, nil
}

func (m *mockPrefixCacheMgr) MatchPrefixByText(_ string, _ int64, _ string, _ []string) (map[string]int, int, error) {
	return m.matched, 0, nil
}

func (m *mockPrefixCacheMgr) AddPrefix(_ string, _ int64, _ string, _ []int64) error {
	m.mu.Lock()
	m.addCalls++
	m.mu.Unlock()
	return nil
}

func (m *mockPrefixCacheMgr) RemoveInstance(_ string) error { return nil }
func (m *mockPrefixCacheMgr) Tokenize(_ context.Context, _ string, _ []prefixcache.ChatMessage, _ string) ([]int64, error) {
	return nil, nil
}
func (m *mockPrefixCacheMgr) TokenizePrompt(_ context.Context, _, _, _ string) ([]int64, error) {
	return nil, nil
}
func (m *mockPrefixCacheMgr) GetStats() prefixcache.PrefixCacheStats {
	return prefixcache.PrefixCacheStats{}
}
func (m *mockPrefixCacheMgr) OnBlockStored(_ kvevents.BlockStored) error           { return nil }
func (m *mockPrefixCacheMgr) OnBlockRemoved(_ kvevents.BlockRemoved) error         { return nil }
func (m *mockPrefixCacheMgr) OnAllBlocksCleared(_ kvevents.AllBlocksCleared) error { return nil }
func (m *mockPrefixCacheMgr) SubscribeInstance(_, _, _, _ string) error            { return nil }
func (m *mockPrefixCacheMgr) UnsubscribeInstance(_ string)                         {}
func (m *mockPrefixCacheMgr) SetRenderClient(_ *renderclient.RenderClient)         {}
func (m *mockPrefixCacheMgr) Stop()                                                {}

// recordingProvider is a metricProvider that captures the MetricQueryOptions it
// was called with (to verify ScheduleOptions propagation, fix #4). It returns a
// fixed set of per-URL metrics; tests mutate TokenNum directly to simulate
// instance.addReq's load accumulation between schedules (fix #1).
type recordingProvider struct {
	byURL    map[string]*InstanceMetric
	mu       sync.Mutex
	lastOpts *MetricQueryOptions // last MetricQueryOptions received
}

func (p *recordingProvider) GetInstanceMetrics(_ []string, opts *MetricQueryOptions) ([]*InstanceMetric, error) {
	p.mu.Lock()
	p.lastOpts = opts
	out := make([]*InstanceMetric, 0, len(p.byURL))
	for _, m := range p.byURL {
		out = append(out, m)
	}
	p.mu.Unlock()
	return out, nil
}

func (p *recordingProvider) RangeMetrics(_ []string, _ *MetricQueryOptions, fn func(*InstanceMetric) bool) error {
	for _, m := range p.byURL {
		if !fn(m) {
			break
		}
	}
	return nil
}

func (p *recordingProvider) AddRequest(_ *LlmRequest, _ *InstanceContext) error    { return nil }
func (p *recordingProvider) RemoveRequest(_ *LlmRequest, _ *InstanceContext) error { return nil }
func (p *recordingProvider) PredictTokensByEMA(_ *LlmRequest) int                  { return 0 }
func (p *recordingProvider) GetDPAwareMetrics(_ *MetricQueryOptions, _ int) ([]*DPAwareMetric, error) {
	return nil, nil
}

// TestPrefixCacheSchedule_WritesBackPredictTokens: schedule() must write
// PredictTokens = PromptLen so instance.addReq accumulates real load. After the
// first request loads one pod, the second request sees a load spread exceeding
// the imbalance threshold and routes to the (now) lowest-load pod — proving the
// imbalance branch can finally fire (carcosa #1). Also asserts schedule() no
// longer optimistically calls AddPrefix (carcosa #2).
func TestPrefixCacheSchedule_WritesBackPredictTokens(t *testing.T) {
	// Two pods, both start at TokenNum 0. Pod B has the higher prefix match.
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 0, 0)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 0, 0)
	byURL := map[string]*InstanceMetric{mA.InsUrl: mA, mB.InsUrl: mB}
	provider := &recordingProvider{byURL: byURL}

	mgr := &mockPrefixCacheMgr{
		urlToName: map[string]string{mA.InsUrl: nameA, mB.InsUrl: nameB},
		matched:   map[string]int{nameA: 30, nameB: 90}, // B matches best
	}
	lb := &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{metricProvider: provider},
		prefixCacheMgr:   mgr,
		// threshold small enough that one prompt's worth of tokens flips the
		// branch: after pod B absorbs a 500-token prompt, spread = 500 > 100.
		loadImbalanceThreshold: 100,
	}
	urls := []string{mA.InsUrl, mB.InsUrl}

	req := &ScheduleRequestMsg{
		Request:              &LlmRequest{ReqId: "r1", PromptLen: 500},
		CandidateInstanceIDs: urls,
	}

	// First schedule: spread = 0 (both pods at 0) -> balanced branch -> highest
	// match pod B.
	r1 := lb.schedule(req, nil)
	assert.NotNil(t, r1)
	assert.Equal(t, mB.InsUrl, r1.PrefillUrl, "1st req: balanced, pick highest-match pod B")

	// #1 assertion: PredictTokens was written back to PromptLen.
	assert.Equal(t, 500, req.Request.PredictTokens, "schedule must set PredictTokens = PromptLen")

	// Simulate instance.addReq on the selected pod (production instance.go:
	// tokenNum += PredictTokens). The dispatcher does this after schedule().
	mB.TokenNum += req.Request.PredictTokens
	assert.Equal(t, 500, mB.TokenNum, "selected pod B accumulated the prompt tokens")
	assert.Equal(t, 0, mA.TokenNum, "unselected pod A still at 0")

	// Second schedule: spread = 500-0 = 500 > 100 -> imbalance branch -> lowest
	// load pod A, even though A has the lower prefix match.
	req2 := &ScheduleRequestMsg{
		Request:              &LlmRequest{ReqId: "r2", PromptLen: 500},
		CandidateInstanceIDs: urls,
	}
	r2 := lb.schedule(req2, nil)
	assert.NotNil(t, r2)
	assert.Equal(t, mA.InsUrl, r2.PrefillUrl,
		"2nd req: imbalanced (spread=500>100), route to lowest-load pod A despite lower match")

	// #2 assertion: schedule no longer optimistically calls AddPrefix.
	mgr.mu.Lock()
	addCalls := mgr.addCalls
	mgr.mu.Unlock()
	assert.Equal(t, 0, addCalls, "AddPrefix must not be called (fix #2)")
}

// TestPrefixCacheSchedule_PropagatesScheduleOptions: selectFromMatched and
// getReadyInstances must thread ScheduleOptions through buildQueryOptions so
// GroupID/ExcludeGroupIDs reach the metric provider (carcosa #4).
func TestPrefixCacheSchedule_PropagatesScheduleOptions(t *testing.T) {
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 1)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1100, 1)
	byURL := map[string]*InstanceMetric{mA.InsUrl: mA, mB.InsUrl: mB}
	provider := &recordingProvider{byURL: byURL}

	mgr := &mockPrefixCacheMgr{
		urlToName: map[string]string{mA.InsUrl: nameA, mB.InsUrl: nameB},
		matched:   map[string]int{nameA: 80, nameB: 90},
	}
	lb := &prefixCacheLoadBalancer{
		baseLoadBalancer:       baseLoadBalancer{metricProvider: provider},
		prefixCacheMgr:         mgr,
		loadImbalanceThreshold: 1000, // balanced -> match branch
	}
	urls := []string{mA.InsUrl, mB.InsUrl}

	opts := &ScheduleOptions{
		GroupID:         "g1",
		ExcludeGroupIDs: map[string]bool{"g-excluded": true},
	}
	req := &ScheduleRequestMsg{
		Request:              &LlmRequest{ReqId: "r1", PromptLen: 10},
		CandidateInstanceIDs: urls,
	}

	_ = lb.schedule(req, opts)

	provider.mu.Lock()
	defer provider.mu.Unlock()
	assert.NotNil(t, provider.lastOpts, "provider must have been queried")
	assert.Equal(t, "g1", provider.lastOpts.GroupID, "GroupID must propagate to provider")
	assert.Equal(t, map[string]bool{"g-excluded": true}, provider.lastOpts.ExcludeGroupIDs,
		"ExcludeGroupIDs must propagate to provider")
}

// TestPrefixCacheFallback_UsesTokenLBNotLeastConn: the degraded fallback path
// must pick the lowest-TokenNum pod, NOT the lowest-ReqNum pod. #899: the prior
// fallback used leastConn (ReqNum), which is uncorrelated with the TokenNum
// load dimension prefixCache optimizes for.
//
// Setup: pod A has lower ReqNum (1) but higher TokenNum (5000); pod B has higher
// ReqNum (9) but lower TokenNum (1000). leastConn would pick A (reqNum 1);
// token LB must pick B (tokenNum 1000). We assert B.
func TestPrefixCacheFallback_UsesTokenLBNotLeastConn(t *testing.T) {
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 1) // low ReqNum, high TokenNum
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 9) // high ReqNum, low TokenNum
	byURL := map[string]*InstanceMetric{mA.InsUrl: mA, mB.InsUrl: mB}
	provider := &recordingProvider{byURL: byURL}

	// matched is empty -> schedule() falls through to lb.fallback(). Build a
	// real token LB as the fallback (mirror newPrefixCacheLB after #899).
	tokenLB, err := newTokenLB(provider, &AlgorithmParams{
		TbtThreshold:  50,
		TtftThreshold: 200,
	})
	assert.NoError(t, err)

	lb := &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{metricProvider: provider},
		prefixCacheMgr: &mockPrefixCacheMgr{
			urlToName: map[string]string{mA.InsUrl: nameA, mB.InsUrl: nameB},
			matched:   map[string]int{}, // no prefix match -> fallback
		},
		fallbackLB: tokenLB,
	}

	req := &ScheduleRequestMsg{
		Request:              &LlmRequest{ReqId: "r1", PromptLen: 10},
		CandidateInstanceIDs: []string{mA.InsUrl, mB.InsUrl},
	}
	r := lb.schedule(req, nil)
	assert.NotNil(t, r)
	assert.Equal(t, mB.InsUrl, r.PrefillUrl,
		"fallback must pick lowest-TokenNum pod B, not leastConn's lowest-ReqNum pod A")
}
