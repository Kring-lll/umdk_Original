/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: unit tests for the prefix-match load-balancing-aware scheduler.
 */

package gs

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

// prefixCacheMockProvider returns fixed per-URL metrics so that each pod can
// have distinct TokenNum / ReqNum values. Unlike mockMetricProvider, it is
// instance-aware: callers seed it with a url->metric map and it returns the
// union of those metrics regardless of the queried IDs (selectFromMatched
// passes all ready URLs and expects all of them back).
type prefixCacheMockProvider struct {
	byURL map[string]*InstanceMetric
}

func (p *prefixCacheMockProvider) GetInstanceMetrics(_ []string, _ *MetricQueryOptions) ([]*InstanceMetric, error) {
	out := make([]*InstanceMetric, 0, len(p.byURL))
	for _, m := range p.byURL {
		out = append(out, m)
	}
	return out, nil
}

func (p *prefixCacheMockProvider) RangeMetrics(_ []string, _ *MetricQueryOptions, fn func(*InstanceMetric) bool) error {
	for _, m := range p.byURL {
		if !fn(m) {
			break
		}
	}
	return nil
}

func (p *prefixCacheMockProvider) AddRequest(_ *LlmRequest, _ *InstanceContext) error    { return nil }
func (p *prefixCacheMockProvider) RemoveRequest(_ *LlmRequest, _ *InstanceContext) error { return nil }
func (p *prefixCacheMockProvider) PredictTokensByEMA(_ *LlmRequest) int                  { return 0 }
func (p *prefixCacheMockProvider) GetDPAwareMetrics(_ *MetricQueryOptions, _ int) ([]*DPAwareMetric, error) {
	return nil, nil
}

// buildPrefixCacheLB constructs a prefixCacheLoadBalancer wired with a mock
// provider, bypassing the kvevents/render machinery so selectFromMatched can
// be tested in isolation.
func buildPrefixCacheLB(metrics map[string]*InstanceMetric, imbalanceThreshold int) *prefixCacheLoadBalancer {
	return &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{
			metricProvider: &prefixCacheMockProvider{byURL: metrics},
		},
		loadImbalanceThreshold: imbalanceThreshold,
	}
}

// makeMetric builds an InstanceMetric for a pod with the given URL/name, token
// load and request count.
func makeMetric(url, name string, tokenNum, reqNum int) (*InstanceMetric, string) {
	return &InstanceMetric{
		InsUrl:   url,
		TokenNum: tokenNum,
		ReqNum:   reqNum,
	}, name
}

// TestWithAlgorithmThreshold_DefaultLoadImbalanceThreshold: when the caller
// passes loadImbalanceThreshold <= 0 (user did not set the knob), the option
// applies the built-in default so the prefixCache imbalance intervention is
// active without surfacing the config to users. See #900.
func TestWithAlgorithmThreshold_DefaultLoadImbalanceThreshold(t *testing.T) {
	gs := &GlobalSchedulerManager{}
	opt := WithAlgorithmThreshold(50, 32, false, 128, 0)
	assert.NoError(t, opt(gs))
	assert.Equal(t, defaultLoadImbalanceThreshold, gs.config.lbConfig.LoadImbalanceThreshold,
		"unset threshold must fall back to default")

	gs2 := &GlobalSchedulerManager{}
	opt2 := WithAlgorithmThreshold(50, 32, false, 128, 5000)
	assert.NoError(t, opt2(gs2))
	assert.Equal(t, 5000, gs2.config.lbConfig.LoadImbalanceThreshold,
		"explicit threshold must be honored")
}

// buildURLMap builds urlToInstanceName from a list of (url,name) pairs.
func buildURLMap(pods ...[2]string) map[string]string {
	m := make(map[string]string, len(pods))
	for _, p := range pods {
		m[p[0]] = p[1]
	}
	return m
}

// TestSelectFromMatched_LoadImbalanced_SelectsLowestLoad: when the token-load
// spread exceeds the threshold, the lowest-load pod is chosen even if it has a
// lower prefix match rate (or none).
func TestSelectFromMatched_LoadImbalanced_SelectsLowestLoad(t *testing.T) {
	// pods: A(5000,80%), B(1000,90%), C(3000,70%); spread = 5000-1000 = 4000 > 1000
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 1)
	mC, nameC := makeMetric("127.0.0.1:3", "worker-3", 3000, 3)

	metrics := map[string]*InstanceMetric{
		"A": mA, "B": mB, "C": mC,
	}
	urlMap := buildURLMap([2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB},
		[2]string{"127.0.0.1:3", nameC})
	matched := map[string]int{nameA: 80, nameB: 90, nameC: 70}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "should pick lowest-load pod B")
}

// TestSelectFromMatched_LoadBalanced_SelectsHighestMatch: under balanced load
// (spread <= threshold), the highest-match-rate pod wins.
func TestSelectFromMatched_LoadBalanced_SelectsHighestMatch(t *testing.T) {
	// pods: A(1000,80%), B(1100,90%), C(1200,70%); spread = 1200-1000 = 200 <= 1000
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1100, 1)
	mC, nameC := makeMetric("127.0.0.1:3", "worker-3", 1200, 3)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB, "C": mC}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB},
		[2]string{"127.0.0.1:3", nameC})
	matched := map[string]int{nameA: 80, nameB: 90, nameC: 70}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "should pick highest-match pod B (90%)")
}

// TestSelectFromMatched_Balanced_TiebreakByLowerLoad: when match rates tie,
// the lower-load pod wins under balanced load.
func TestSelectFromMatched_Balanced_TiebreakByLowerLoad(t *testing.T) {
	// pods: A(1000,80%), B(800,80%), C(1200,70%); A and B tie at 80% -> B (800<1000)
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 800, 9)
	mC, nameC := makeMetric("127.0.0.1:3", "worker-3", 1200, 3)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB, "C": mC}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB},
		[2]string{"127.0.0.1:3", nameC})
	matched := map[string]int{nameA: 80, nameB: 80, nameC: 70}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "should tiebreak to lower-load pod B (800)")
}

// TestSelectFromMatched_Balanced_LowMatchStillSelectsTopMatch: under balanced
// load, even a low match rate wins (partial KV reuse beats leastConn) — the
// old matchThreshold gate is gone, so the top-match pod is always returned.
func TestSelectFromMatched_Balanced_LowMatchStillSelectsTopMatch(t *testing.T) {
	// pods: A(1000,30%), B(1100,20%); balanced -> pick A (30% > 20%)
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1100, 1)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB})
	matched := map[string]int{nameA: 30, nameB: 20}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:1", got.InsUrl, "low match still selects top-match pod A")
}

// TestSelectFromMatched_Imbalanced_SelectsUnmatchedLowestLoad: when load is
// imbalanced, the lowest-load pod is chosen even if it has NO prefix match.
func TestSelectFromMatched_Imbalanced_SelectsUnmatchedLowestLoad(t *testing.T) {
	// pods: A(5000,90%), B(1000, no match); spread = 4000 > 1000 -> pick B
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 1)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB})
	matched := map[string]int{nameA: 90} // only A matched; B is unmatched

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "should pick unmatched lowest-load pod B")
}

// TestSelectFromMatched_ThresholdZero_DisablesImbalanceBranch: with
// loadImbalanceThreshold=0, the imbalance branch is skipped entirely and
// selection falls through to the prefix-match path.
func TestSelectFromMatched_ThresholdZero_DisablesImbalanceBranch(t *testing.T) {
	// pods: A(5000,90%), B(1000,80%); spread=4000 but threshold=0 -> prefix path
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 1)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB})
	matched := map[string]int{nameA: 90, nameB: 80}

	lb := buildPrefixCacheLB(metrics, 0)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:1", got.InsUrl,
		"threshold=0 disables imbalance branch -> pick highest-match A (90%)")
}

// TestSelectFromMatched_SinglePod_FallsToBalancedBranch: a single pod has
// spread=0 (maxIdx==minIdx==0), so it never enters the imbalance branch and
// returns the only pod if it meets the match gate.
func TestSelectFromMatched_SinglePod_FallsToBalancedBranch(t *testing.T) {
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 5)

	metrics := map[string]*InstanceMetric{"A": mA}
	urlMap := buildURLMap([2]string{"127.0.0.1:1", nameA})
	matched := map[string]int{nameA: 90}

	lb := buildPrefixCacheLB(metrics, 1000) // 1000>0 but single pod: spread=0
	urls := []string{"127.0.0.1:1"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:1", got.InsUrl, "single pod above gate should be selected")
}

// TestSelectFromMatched_AllPodsEqualTokenNum: when every pod has the same
// TokenNum, spread=0 -> balanced branch -> pick highest match.
func TestSelectFromMatched_AllPodsEqualTokenNum(t *testing.T) {
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 3)
	mC, nameC := makeMetric("127.0.0.1:3", "worker-3", 1000, 9)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB, "C": mC}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB},
		[2]string{"127.0.0.1:3", nameC})
	matched := map[string]int{nameA: 80, nameB: 90, nameC: 70}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "equal load -> highest match B (90%)")
}

// TestSelectFromMatched_EqualTokenNumTiebreak_Deterministic: when match%
// AND TokenNum both tie, the deterministic instanceName tiebreak must pick a
// fixed pod (the lexicographically smaller name).
func TestSelectFromMatched_EqualTokenNumTiebreak_Deterministic(t *testing.T) {
	// worker-1 and worker-3 tie at 80% / load 1000; worker-1 < worker-3 lexically.
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-3", 1000, 3)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB})
	matched := map[string]int{nameA: 80, nameB: 80}

	lb := buildPrefixCacheLB(metrics, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:1", got.InsUrl,
		"full tie -> deterministic instanceName tiebreak picks worker-1 (lexically smaller)")
}

// filteredMockProvider returns metrics only for requested URLs, simulating a
// real provider that may return FEWER metrics than ready instances (e.g., a
// freshly registered pod whose metrics aren't collected yet).
type filteredMockProvider struct {
	byURL map[string]*InstanceMetric
}

func (p *filteredMockProvider) GetInstanceMetrics(ids []string, _ *MetricQueryOptions) ([]*InstanceMetric, error) {
	out := make([]*InstanceMetric, 0, len(ids))
	for _, id := range ids {
		if m, ok := p.byURL[id]; ok {
			out = append(out, m)
		}
	}
	return out, nil
}

func (p *filteredMockProvider) RangeMetrics(_ []string, _ *MetricQueryOptions, fn func(*InstanceMetric) bool) error {
	for _, m := range p.byURL {
		if !fn(m) {
			break
		}
	}
	return nil
}

func (p *filteredMockProvider) AddRequest(_ *LlmRequest, _ *InstanceContext) error    { return nil }
func (p *filteredMockProvider) RemoveRequest(_ *LlmRequest, _ *InstanceContext) error { return nil }
func (p *filteredMockProvider) PredictTokensByEMA(_ *LlmRequest) int                  { return 0 }
func (p *filteredMockProvider) GetDPAwareMetrics(_ *MetricQueryOptions, _ int) ([]*DPAwareMetric, error) {
	return nil, nil
}

// TestSelectFromMatched_PartialMetrics: the provider returns metrics for only
// a subset of ready URLs. The missing pod is skipped; min/max are computed on
// the returned subset and the algorithm still selects correctly (no panic).
func TestSelectFromMatched_PartialMetrics(t *testing.T) {
	// ready: A, B, C; provider only has A and B (C missing -> skipped).
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 5000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1000, 1)

	byURL := map[string]*InstanceMetric{
		"127.0.0.1:1": mA,
		"127.0.0.1:2": mB,
		// 127.0.0.1:3 absent -> provider returns nothing for it
	}
	lb := &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{
			metricProvider: &filteredMockProvider{byURL: byURL},
		},
		loadImbalanceThreshold: 1000,
	}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB},
		[2]string{"127.0.0.1:3", "worker-3"}) // ready but no metric
	matched := map[string]int{nameA: 90, nameB: 80} // worker-3 unmatched anyway
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap, nil)
	assert.NotNil(t, got)
	// spread over {A(5000),B(1000)} = 4000 > 1000 -> lowest load -> B(1000)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl,
		"partial metrics: missing pod skipped, imbalance -> lowest-load B")
}
