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
func buildPrefixCacheLB(metrics map[string]*InstanceMetric, matchThreshold, imbalanceThreshold int) *prefixCacheLoadBalancer {
	return &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{
			metricProvider: &prefixCacheMockProvider{byURL: metrics},
		},
		matchThreshold:         matchThreshold,
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

	lb := buildPrefixCacheLB(metrics, 50, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap)
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

	lb := buildPrefixCacheLB(metrics, 50, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap)
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

	lb := buildPrefixCacheLB(metrics, 50, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2", "127.0.0.1:3"}

	got := lb.selectFromMatched(urls, matched, urlMap)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:2", got.InsUrl, "should tiebreak to lower-load pod B (800)")
}

// TestSelectFromMatched_BelowThresholdGate_DeferredToFallback: when load is
// balanced but no pod reaches matchThreshold, returns nil so schedule() falls
// back to leastConn (per design decision Q2).
func TestSelectFromMatched_BelowThresholdGate_DeferredToFallback(t *testing.T) {
	// pods: A(1000,30%), B(1100,20%); balanced, but match < threshold 50
	mA, nameA := makeMetric("127.0.0.1:1", "worker-1", 1000, 5)
	mB, nameB := makeMetric("127.0.0.1:2", "worker-2", 1100, 1)

	metrics := map[string]*InstanceMetric{"A": mA, "B": mB}
	urlMap := buildURLMap(
		[2]string{"127.0.0.1:1", nameA},
		[2]string{"127.0.0.1:2", nameB})
	matched := map[string]int{nameA: 30, nameB: 20}

	lb := buildPrefixCacheLB(metrics, 50, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap)
	assert.Nil(t, got, "should defer to fallback when match below gate")
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

	lb := buildPrefixCacheLB(metrics, 50, 1000)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap)
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

	lb := buildPrefixCacheLB(metrics, 50, 0)
	urls := []string{"127.0.0.1:1", "127.0.0.1:2"}

	got := lb.selectFromMatched(urls, matched, urlMap)
	assert.NotNil(t, got)
	assert.Equal(t, "127.0.0.1:1", got.InsUrl,
		"threshold=0 disables imbalance branch -> pick highest-match A (90%)")
}
