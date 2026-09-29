/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for engine-signal admission control —
 * waiting-gate, burst cap, hysteresis, degraded fallback (T = highWater or
 * batchSize), LB candidate filter, Gate A bypass.
 * Create: 2026-09-22
 */

package gs

import (
	"context"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/stats"
)

func newTestAdmission(t *testing.T, mode string, reqNum func(string) int) *KvAdmissionController {
	t.Helper()
	if reqNum == nil {
		reqNum = func(string) int { return 0 }
	}
	return NewKvAdmissionController(base.KvAdmissionConfig{
		Mode:                 mode,
		MetricsURL:           "http://127.0.0.1:1027/metrics?type=instance",
		PollMs:               1000,
		BurstCapPerCycle:     1,
		HysteresisCycles:     1,
		StaleAfterMs:         8000,
		RecoverHealthyCycles: 2,
	}, 8, reqNum, func() int { return 1 })
}

// pollApply applies a parsed signals map the way a successful poll would
// (test harness): single-entry signals become the operating signal and a
// fresh quota epoch starts.
func (c *KvAdmissionController) pollApply(sigs map[string]engineSignal) {
	c.mu.Lock()
	defer c.mu.Unlock()
	// Mirror pollOnce: multiple instance signals under the single test
	// registration aggregate into one admission unit (never multiInstance —
	// the test controller reports one aigw registration).
	if len(sigs) == 1 {
		for _, s := range sigs {
			c.signal = s
		}
	} else if len(sigs) > 1 {
		c.signal = aggregateSignals(sigs)
	}
	c.multiInstance = false
	c.lastOk = time.Now()
	if c.healthyStreak < c.recoverCyc {
		c.healthyStreak = c.recoverCyc
	}
	c.admits = map[string]int{}
	c.lastQuotaSignal = c.signal
	c.quotaResetAt = time.Now()
}

// markHealthy injects fresh healthy signals (test harness for the poller).
func (c *KvAdmissionController) markHealthy(waiting int) {
	c.mu.Lock()
	defer c.mu.Unlock()
	free := 1
	if waiting > 0 {
		free = 0
	}
	c.signal = engineSignal{
		running: 8 - waiting, waiting: waiting,
		engines: 1, mixedEngines: 1, freeMixed: free,
	}
	c.multiInstance = false
	c.lastOk = time.Now()
	if c.healthyStreak < c.recoverCyc {
		c.healthyStreak = c.recoverCyc
	}
	c.admits = map[string]int{} // poll tick: new cycle
	if waiting > 0 {
		c.pauseRemaining = c.hysteresis + 1
	} else if c.pauseRemaining > 0 {
		c.pauseRemaining--
	}
}

func TestParseEngineSignals(t *testing.T) {
	body := []byte(`# HELP vllm:num_requests_running Number of running requests
# TYPE vllm:num_requests_running gauge
vllm:num_requests_running{instance_id="0",model_name="qwen"} 8
vllm:num_requests_waiting{instance_id="0",model_name="qwen"} 5
vllm:kv_cache_usage_perc{instance_id="0"} 0.963
vllm:num_requests_waiting_by_reason{instance_id="0",reason="capacity"} 2
motor:active_prefill_workers 3
unlabeled_metric 42
`)
	sigs := parseEngineSignals(body)
	s, ok := sigs["0"]
	assert.True(t, ok)
	assert.Equal(t, 8, s.running)
	assert.Equal(t, 5, s.waiting)
	assert.InDelta(t, 0.963, s.kvUsage, 0.0001)
	assert.Len(t, sigs, 1, "by_reason must not create a second signal; unlabeled metrics are ignored")
}

// TestParseEngineSignalsVLLMBareAggregation: vLLM bare /metrics under
// --data-parallel-size exposes one line per DP engine (engine="N" label, no
// instance_id). The engines must aggregate into ONE pseudo-instance —
// counters summed, utilization maxed — so any engine's activity changes the
// versioned-quota snapshot instead of freezing on the last engine's values.
func TestParseEngineSignalsVLLMBareAggregation(t *testing.T) {
	body := []byte(`vllm:num_requests_running{engine="0",model_name="ds"} 1
vllm:num_requests_running{engine="1",model_name="ds"} 2
vllm:num_requests_running{engine="7",model_name="ds"} 0
vllm:num_requests_waiting{engine="0",model_name="ds"} 0
vllm:num_requests_waiting{engine="1",model_name="ds"} 1
vllm:gpu_cache_usage_perc{engine="0",model_name="ds"} 0.3
vllm:gpu_cache_usage_perc{engine="1",model_name="ds"} 0.9
vllm:prefix_cache_queries_total{engine="0",model_name="ds"} 100
vllm:prefix_cache_queries_total{engine="1",model_name="ds"} 50
`)
	sigs := parseEngineSignals(body)
	assert.Len(t, sigs, 1, "all engines aggregate into one pseudo-instance")
	s, ok := sigs[""]
	assert.True(t, ok)
	assert.Equal(t, 3, s.running, "running sums across engines")
	assert.Equal(t, 1, s.waiting, "waiting sums across engines")
	assert.InDelta(t, 0.9, s.kvUsage, 0.0001, "kvUsage takes the most loaded engine")
	assert.InDelta(t, 150.0, s.queriesTotal, 0.0001, "queriesTotal sums across engines")

	// Version property: any single engine's activity must change the
	// aggregated snapshot, so the versioned quota refreshes no matter which
	// DP engine the coordinator routes the request to.
	body2 := []byte(`vllm:num_requests_running{engine="0",model_name="ds"} 2
vllm:num_requests_running{engine="1",model_name="ds"} 2
vllm:num_requests_running{engine="7",model_name="ds"} 0
vllm:num_requests_waiting{engine="0",model_name="ds"} 0
vllm:num_requests_waiting{engine="1",model_name="ds"} 1
vllm:gpu_cache_usage_perc{engine="0",model_name="ds"} 0.3
vllm:gpu_cache_usage_perc{engine="1",model_name="ds"} 0.9
vllm:prefix_cache_queries_total{engine="0",model_name="ds"} 100
vllm:prefix_cache_queries_total{engine="1",model_name="ds"} 50
`)
	sigs2 := parseEngineSignals(body2)
	assert.NotEqual(t, sigs[""], sigs2[""],
		"one engine's running change must change the aggregated signal (quota version refresh)")
}

// TestParseEngineSignalsSingleEngineUnlabeled: a single-engine metrics source
// without instance_id or engine labels keeps exact values (sum of one line).
func TestParseEngineSignalsSingleEngineUnlabeled(t *testing.T) {
	body := []byte(`vllm:num_requests_running{model_name="qwen"} 4
vllm:num_requests_waiting{model_name="qwen"} 2
vllm:kv_cache_usage_perc 0.5
`)
	sigs := parseEngineSignals(body)
	s, ok := sigs[""]
	assert.True(t, ok)
	assert.Equal(t, 4, s.running)
	assert.Equal(t, 2, s.waiting)
	assert.InDelta(t, 0.5, s.kvUsage, 0.0001)
}

func TestSignalGateWaiting(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.markHealthy(0)
	assert.True(t, c.AllowDispatch("http://ins-1"), "clear engine admits")

	c.markHealthy(3)
	assert.False(t, c.AllowDispatch("http://ins-1"), "engine waiting>0 must hold dispatch")
}

func TestSignalGateBurstCap(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.markHealthy(0)
	assert.True(t, c.AllowDispatch("http://ins-1"))
	assert.False(t, c.AllowDispatch("http://ins-1"), "burst cap K=1: second admit in the same cycle denied")

	c.markHealthy(0) // new cycle resets the cap
	assert.True(t, c.AllowDispatch("http://ins-1"))
}

// TestBurstCapAutoScalesWithEngines: burstCapPerCycle=0 (auto) must admit
// one per engine per cycle under bare-metrics aggregation — the DP cluster
// is one pseudo-instance, so a fixed default of 1 would throttle the whole
// cluster to one admit per poll.
func TestBurstCapAutoScalesWithEngines(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.burstCap = 0 // auto
	c.markHealthy(0)
	c.pollApply(parseEngineSignals([]byte(
		`vllm:num_requests_running{engine="0"} 1
vllm:num_requests_running{engine="1"} 1
vllm:num_requests_running{engine="2"} 1
vllm:num_requests_running{engine="3"} 1
vllm:num_requests_running{engine="4"} 1
vllm:num_requests_running{engine="5"} 1
vllm:num_requests_running{engine="6"} 1
vllm:num_requests_running{engine="7"} 1
vllm:prefix_cache_queries_total{engine="0"} 10
vllm:prefix_cache_queries_total{engine="1"} 11
vllm:prefix_cache_queries_total{engine="2"} 12
vllm:prefix_cache_queries_total{engine="3"} 13
vllm:prefix_cache_queries_total{engine="4"} 14
vllm:prefix_cache_queries_total{engine="5"} 15
vllm:prefix_cache_queries_total{engine="6"} 16
vllm:prefix_cache_queries_total{engine="7"} 17
`)))
	assert.Equal(t, 8, c.signal.engines, "distinct engine labels are counted")

	allowed := 0
	for i := 0; i < 12; i++ {
		if c.AllowDispatch("http://ins-1") {
			allowed++
		}
	}
	assert.Equal(t, 8, allowed, "auto cap admits one per engine per cycle")
}

// TestBurstCapExplicitOverridesAuto: an explicit burstCapPerCycle wins over
// the engine-count auto default.
func TestBurstCapExplicitOverridesAuto(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.burstCap = 2 // explicit
	c.markHealthy(0)
	c.pollApply(parseEngineSignals([]byte(
		`vllm:num_requests_running{engine="0"} 1
vllm:num_requests_running{engine="1"} 1
`)))
	allowed := 0
	for i := 0; i < 6; i++ {
		if c.AllowDispatch("http://ins-1") {
			allowed++
		}
	}
	assert.Equal(t, 2, allowed, "explicit cap is respected even with engines>1")
}

// TestBurstCapAutoMotorDefaultsToOne: motor-style instance_id signals carry
// no engine labels — auto stays 1 per instance per cycle.
func TestBurstCapAutoMotorDefaultsToOne(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.burstCap = 0
	c.markHealthy(0)
	c.pollApply(parseEngineSignals([]byte(
		`vllm:num_requests_running{instance_id="0"} 1
vllm:prefix_cache_queries_total{instance_id="0"} 10
`)))
	// The engine census counts the single motor endpoint as one engine; the
	// free count (not the raw topology) drives the auto cap.
	assert.Equal(t, 1, c.signal.engines)
	assert.Equal(t, 1, c.signal.freeMixed)
	assert.True(t, c.AllowDispatch("http://ins-1"))
	assert.False(t, c.AllowDispatch("http://ins-1"), "motor auto cap is 1 per instance")
}

// TestPDCapacityTakesRoleMin: a PD unit only has capacity when BOTH roles
// have a free engine; the count is the min of the free engines per role.
func TestPDCapacityTakesRoleMin(t *testing.T) {
	// 2 prefill engines (1 busy) + 3 decode engines (all free): capacity 1.
	sigs := parseEngineSignals([]byte(
		`vllm:num_requests_waiting{dp_rank="0",role="prefill",instance_id="0"} 0
vllm:num_requests_waiting{dp_rank="1",role="prefill",instance_id="0"} 2
vllm:num_requests_waiting{dp_rank="0",role="decode",instance_id="1"} 0
vllm:num_requests_waiting{dp_rank="1",role="decode",instance_id="1"} 0
vllm:num_requests_waiting{dp_rank="2",role="decode",instance_id="1"} 0
`))
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.pollApply(sigs)
	assert.Equal(t, 2, c.signal.prefillEngines)
	assert.Equal(t, 3, c.signal.decodeEngines)
	assert.Equal(t, 1, c.signal.freePrefill, "one prefill engine is busy")
	assert.Equal(t, 3, c.signal.freeDecode)
	assert.Equal(t, 1, c.signal.capacity(), "PD capacity is min(freeP, freeD)")

	// All prefill busy → zero capacity → hold even though decode is free.
	sigs2 := parseEngineSignals([]byte(
		`vllm:num_requests_waiting{dp_rank="0",role="prefill",instance_id="0"} 1
vllm:num_requests_waiting{dp_rank="1",role="prefill",instance_id="0"} 2
vllm:num_requests_waiting{dp_rank="0",role="decode",instance_id="1"} 0
`))
	c2 := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c2.pollApply(sigs2)
	assert.Zero(t, c2.signal.capacity(), "no prefill capacity means no dispatch")
	assert.False(t, c2.AllowDispatch("http://ins-1"))
}

// TestMotorDPWithinInstanceAggregates: multiple dp_rank lines under one
// instance_id must aggregate (sum), not overwrite each other — the latent
// bug the bare-metrics fix also covered for the motor format.
func TestMotorDPWithinInstanceAggregates(t *testing.T) {
	sigs := parseEngineSignals([]byte(
		`vllm:num_requests_running{dp_rank="0",role="mixed",instance_id="0"} 1
vllm:num_requests_running{dp_rank="1",role="mixed",instance_id="0"} 2
vllm:num_requests_running{dp_rank="2",role="mixed",instance_id="0"} 3
vllm:num_requests_waiting{dp_rank="0",role="mixed",instance_id="0"} 0
vllm:num_requests_waiting{dp_rank="1",role="mixed",instance_id="0"} 1
vllm:num_requests_waiting{dp_rank="2",role="mixed",instance_id="0"} 0
`))
	s := sigs["0"]
	assert.Equal(t, 6, s.running, "running sums across dp_ranks")
	assert.Equal(t, 1, s.waiting, "waiting sums across dp_ranks")
	assert.Equal(t, 3, s.engines)
	assert.Equal(t, 2, s.freeMixed, "two of three engines are free")
	assert.Equal(t, 2, s.capacity())
}

// TestMultiInstanceAggregatesWhenSingleRegistration: one aigw registration
// fronting multiple motor instances (PD pods) aggregates them into a single
// admission unit with the PD min semantics preserved.
func TestMultiInstanceAggregatesWhenSingleRegistration(t *testing.T) {
	sigs := parseEngineSignals([]byte(
		`vllm:num_requests_waiting{dp_rank="0",role="prefill",instance_id="0"} 0
vllm:num_requests_waiting{dp_rank="0",role="decode",instance_id="1"} 0
vllm:num_requests_waiting{dp_rank="1",role="decode",instance_id="1"} 3
`))
	assert.Len(t, sigs, 2, "two motor instances parsed separately")

	c := newTestAdmission(t, base.KvAdmissionSignal, nil) // instanceCount = 1
	c.pollApply(sigs)
	assert.Equal(t, 1, c.signal.prefillEngines)
	assert.Equal(t, 2, c.signal.decodeEngines)
	assert.Equal(t, 1, c.signal.freeDecode, "one decode engine is busy")
	assert.Equal(t, 1, c.signal.capacity(), "aggregated PD unit takes the role min")
	assert.False(t, c.multiInstance, "single registration must not degrade")
}

// TestPartialAvailabilityAdmits: one busy engine among free ones must not
// hold the whole unit — the free engines still admit (per-engine gating,
// replacing the old any-waiting-holds-all semantics).
func TestPartialAvailabilityAdmits(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.burstCap = 0 // auto
	c.pollApply(parseEngineSignals([]byte(
		`vllm:num_requests_waiting{engine="0"} 0
vllm:num_requests_waiting{engine="1"} 5
vllm:num_requests_waiting{engine="2"} 0
vllm:num_requests_waiting{engine="3"} 0
`)))
	assert.Equal(t, 3, c.signal.capacity(), "three of four engines are free")
	allowed := 0
	for i := 0; i < 5; i++ {
		if c.AllowDispatch("http://ins-1") {
			allowed++
		}
	}
	assert.Equal(t, 3, allowed, "auto cap admits the free-engine count, not zero")
}

func TestSignalGateHysteresis(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.markHealthy(2)
	assert.False(t, c.AllowDispatch("http://ins-1"))
	c.markHealthy(0)
	assert.False(t, c.AllowDispatch("http://ins-1"), "first clear cycle still paused (hysteresis=1)")
	c.markHealthy(0)
	assert.True(t, c.AllowDispatch("http://ins-1"))
}

func TestDegradedGateUsesHighWater(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	// No healthy signals ever: cold start falls back to batchSize.
	inFlight := 7
	c.reqNum = func(string) int { return inFlight }
	assert.True(t, c.AllowDispatch("http://ins-1"), "7 < batchSize 8 admits")
	inFlight = 8
	assert.False(t, c.AllowDispatch("http://ins-1"), "cold start: 8 >= batchSize 8 denies (and samples highWater=8)")

	// highWater learned at 8 replaces batchSize as T.
	inFlight = 7
	assert.True(t, c.AllowDispatch("http://ins-1"), "T=highWater=8: 7 admits")
	inFlight = 8
	assert.False(t, c.AllowDispatch("http://ins-1"))

	// A higher operating point raises T (the sampling is dispatch-time).
	inFlight = 12
	assert.False(t, c.AllowDispatch("http://ins-1"))
	inFlight = 11
	assert.True(t, c.AllowDispatch("http://ins-1"), "T=highWater=12: 11 admits")
}

func TestLBCandidateFilter(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.markHealthy(1)
	assert.False(t, c.LBCandidate("http://ins-1"), "fresh signals: saturated instance filtered")

	c.healthyStreak = 0 // degrade
	assert.True(t, c.LBCandidate("http://ins-1"), "degraded: filter is a no-op")
}

func TestMultiInstanceDegrades(t *testing.T) {
	sigs := parseEngineSignals([]byte(
		`vllm:num_requests_running{instance_id="0"} 3
vllm:num_requests_running{instance_id="1"} 5
`))
	assert.Len(t, sigs, 2)

	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.mu.Lock()
	c.multiInstance = true
	c.mu.Unlock()
	assert.True(t, c.LBCandidate("http://ins-1"), "unmappable multi-instance signals degrade the filter")
}

func TestDegradationStats(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.AllowDispatch("http://ins-1") // degraded (no signals yet)
	count, total := c.DegradationStats()
	assert.Equal(t, int64(1), count)
	assert.True(t, total >= 0)

	c.markHealthy(0)
	c.AllowDispatch("http://ins-1") // healthy again
	count, total = c.DegradationStats()
	assert.Equal(t, int64(1), count, "no new episode")
	assert.True(t, total > 0, "episode duration accumulated")
}

func TestPollerEndToEnd(t *testing.T) {
	var served atomic.Int64
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		served.Add(1)
		w.Write([]byte(`vllm:num_requests_running{instance_id="0"} 4
vllm:num_requests_waiting{instance_id="0"} 0
`))
	}))
	defer srv.Close()

	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.metricsURL = srv.URL
	c.pollInterval = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var wg sync.WaitGroup
	c.Start(ctx, &wg)

	deadline := time.Now().Add(2 * time.Second)
	for served.Load() < 2 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	assert.GreaterOrEqual(t, served.Load(), int64(2), "poller must poll repeatedly")
	assert.True(t, c.AllowDispatch("http://ins-1"), "fresh polled signals admit")
	cancel()
	wg.Wait()
}

func TestGateABypassedUnderAdmission(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.metricProvider = starvationTestMetric{}
	m.capacityProvider = NewCapacityProvider(1, func() int { return 1 })
	m.stats = stats.NewDataPlaneStats()

	// Fill the global capacity gate: original logic would stop dispatching.
	m.capacityProvider.OnDispatched()
	assert.False(t, m.capacityProvider.HasCapacity())

	// With engine-signal admission active and the engine healthy, the
	// bypassed Gate A must not block dispatch.
	m.kvAdmission = newTestAdmission(t, base.KvAdmissionSignal, nil)
	m.kvAdmission.markHealthy(0)

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "gate-a-bypass"}},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "gate-a-bypass",
		SessionID: "sess-gate-a",
		CurStep:   1,
		Msg:       msg,
	}))
	m.tryDispatchFromQueue()
	select {
	case res := <-rsp:
		sug, ok := res.(*SuggestionResultMsg)
		assert.True(t, ok)
		assert.Equal(t, "http://ins-1", sug.PrefillUrl)
	default:
		t.Fatal("dispatch must proceed with Gate A bypassed under admission")
	}
}

func TestAdmissionGateHoldsRequest(t *testing.T) {
	m := newMejTestGSM(t)
	m.scheduler = starvationTestLB{}
	m.metricProvider = starvationTestMetric{}
	m.stats = stats.NewDataPlaneStats()

	m.kvAdmission = newTestAdmission(t, base.KvAdmissionSignal, nil)
	m.kvAdmission.markHealthy(3) // engine saturated

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request:  &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "held"}},
		Response: rsp,
	}
	assert.NoError(t, m.pendingQueue.Add(&PendingRequest{
		ReqID:     "held",
		SessionID: "sess-held",
		CurStep:   1,
		Msg:       msg,
	}))
	m.tryDispatchFromQueue()
	select {
	case <-rsp:
		t.Fatal("saturated engine must hold the request in queue, not dispatch")
	default:
	}
	assert.Equal(t, 1, m.pendingQueue.Size(), "request stays queued")

	m.kvAdmission.markHealthy(0) // first clear cycle: still paused (hysteresis)
	m.tryDispatchFromQueue()
	select {
	case <-rsp:
		t.Fatal("hysteresis must hold one clear cycle")
	default:
	}
	m.kvAdmission.markHealthy(0) // second clear cycle: resume
	m.tryDispatchFromQueue()
	select {
	case res := <-rsp:
		_, ok := res.(*SuggestionResultMsg)
		assert.True(t, ok, "dispatched once the engine drains")
	default:
		t.Fatal("drained engine must dispatch the held request")
	}
}

func TestKvAdmissionRequiresQueueMode(t *testing.T) {
	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "passthrough-model"},
	}
	err := m.initKvAdmission(&base.KvAdmissionConfig{Mode: base.KvAdmissionSignal,
		MetricsURL: "http://127.0.0.1:1027/metrics"}, 8, nil)
	assert.NoError(t, err)
	assert.Nil(t, m.kvAdmission, "passthrough mode (no queue): controller stays inactive")
}

func TestRRCandidateFilterSkipsSaturated(t *testing.T) {
	ins := map[string]int{"http://a": 2, "http://b": 3} // insUrl -> reqNum
	provider := stubMetricProvider{metrics: []*InstanceMetric{
		{InsUrl: "http://a", ReqNum: ins["http://a"]},
		{InsUrl: "http://b", ReqNum: ins["http://b"]},
	}}
	lb, err := newRoundRobinLB(provider, &AlgorithmParams{BatchSize: 8})
	assert.NoError(t, err)

	// Original predicate: both under batchSize → rotation hits both.
	rrReq := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "rr-req"}}
	seen := map[string]bool{}
	for i := 0; i < 4; i++ {
		res := lb.schedule(rrReq, nil)
		seen[res.PrefillUrl] = true
	}
	assert.Len(t, seen, 2)

	// Injected filter: instance a saturated → only b.
	lb.SetCandidateFilter(func(url string) bool { return url != "http://a" })
	seen = map[string]bool{}
	for i := 0; i < 4; i++ {
		res := lb.schedule(rrReq, nil)
		seen[res.PrefillUrl] = true
	}
	assert.Equal(t, map[string]bool{"http://b": true}, seen)
}

type stubMetricProvider struct {
	metrics []*InstanceMetric
}

func (p stubMetricProvider) GetInstanceMetrics([]string, *MetricQueryOptions) ([]*InstanceMetric, error) {
	return p.metrics, nil
}
func (p stubMetricProvider) RangeMetrics(_ []string, _ *MetricQueryOptions, fn func(*InstanceMetric) bool) error {
	for _, m := range p.metrics {
		if !fn(m) {
			break
		}
	}
	return nil
}
func (p stubMetricProvider) AddRequest(*LlmRequest, *InstanceContext) error    { return nil }
func (p stubMetricProvider) RemoveRequest(*LlmRequest, *InstanceContext) error { return nil }
func (p stubMetricProvider) PredictTokensByEMA(*LlmRequest) int                { return 0 }
func (p stubMetricProvider) GetDPAwareMetrics(_ *MetricQueryOptions, _ int) ([]*DPAwareMetric, error) {
	return nil, nil
}

// --- tests for the version-gated quota (one snapshot consumed at most once) ---

// TestVersionGatedQuota_SameSnapshotNoReset: two polls reading the same
// motor cache refresh must NOT grant a second quota window — the burst cap
// stays exhausted until the snapshot content changes.
func TestVersionGatedQuota_SameSnapshotNoReset(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.quotaAgeFallback = time.Hour // disable age fallback for this test

	// First poll: fresh signal grants quota.
	c.mu.Lock()
	c.signal = engineSignal{running: 5, waiting: 0, kvUsage: 0.5, queriesTotal: 100, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.healthyStreak = c.recoverCyc
	c.mu.Unlock()
	c.pollVersionReset()

	// Consume the burst cap.
	assert.True(t, c.AllowDispatch("http://ins-1"))
	assert.False(t, c.AllowDispatch("http://ins-1"), "burst cap exhausted after K=1")

	// Second poll with IDENTICAL signal (motor cache not refreshed):
	// quota must NOT reset.
	c.mu.Lock()
	c.signal = engineSignal{running: 5, waiting: 0, kvUsage: 0.5, queriesTotal: 100, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.mu.Unlock()
	c.pollVersionReset()

	assert.False(t, c.AllowDispatch("http://ins-1"),
		"same snapshot must not grant new quota")
}

// TestVersionGatedQuota_ChangedSnapshotResets: a new motor refresh (any
// gauge change) grants a fresh quota window.
func TestVersionGatedQuota_ChangedSnapshotResets(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.quotaAgeFallback = time.Hour

	// First snapshot.
	c.mu.Lock()
	c.signal = engineSignal{running: 5, waiting: 0, kvUsage: 0.5, queriesTotal: 100, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.healthyStreak = c.recoverCyc
	c.mu.Unlock()
	c.pollVersionReset()

	assert.True(t, c.AllowDispatch("http://ins-1"))
	assert.False(t, c.AllowDispatch("http://ins-1"), "cap exhausted")

	// New refresh: queriesTotal incremented (prefill happened).
	c.mu.Lock()
	c.signal = engineSignal{running: 4, waiting: 0, kvUsage: 0.48, queriesTotal: 101, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.mu.Unlock()
	c.pollVersionReset()

	assert.True(t, c.AllowDispatch("http://ins-1"),
		"changed snapshot must grant new quota")
}

// TestVersionGatedQuota_AgeFallback: a perfectly static engine (no gauge
// change) still gets a forced quota reset after the configured fallback
// window, so a backlog keeps draining.
func TestVersionGatedQuota_AgeFallback(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)
	c.quotaAgeFallback = 50 * time.Millisecond

	// First snapshot.
	c.mu.Lock()
	c.signal = engineSignal{running: 5, waiting: 0, kvUsage: 0.5, queriesTotal: 100, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.healthyStreak = c.recoverCyc
	c.mu.Unlock()
	c.pollVersionReset()

	assert.True(t, c.AllowDispatch("http://ins-1"))
	assert.False(t, c.AllowDispatch("http://ins-1"), "cap exhausted")

	// Age out the quota window.
	time.Sleep(60 * time.Millisecond)

	// Same signal, but quotaResetAt is now past the fallback.
	c.mu.Lock()
	c.signal = engineSignal{running: 5, waiting: 0, kvUsage: 0.5, queriesTotal: 100, engines: 1, mixedEngines: 1, freeMixed: 1}
	c.lastOk = time.Now()
	c.mu.Unlock()
	c.pollVersionReset()

	assert.True(t, c.AllowDispatch("http://ins-1"),
		"age fallback must grant quota on a static engine")
}

// pollVersionReset isolates the version-gating logic from pollOnce for
// direct testing (same code path, without the HTTP fetch).
func (c *KvAdmissionController) pollVersionReset() {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.signal != c.lastQuotaSignal || time.Since(c.quotaResetAt) >= c.quotaAgeFallback {
		c.admits = map[string]int{}
		c.lastQuotaSignal = c.signal
		c.quotaResetAt = time.Now()
	}
}

// --- tests for the dispatch kick (poller drives retries) ---

// TestKickIfOpen_TriggersWhenGateOpen: the kick callback fires when the
// gate is open (healthy, no waiting, no hysteresis pause).
func TestKickIfOpen_TriggersWhenGateOpen(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)

	var kicked int32
	c.SetDispatchKick(func() { atomic.AddInt32(&kicked, 1) })

	// Gate open.
	c.markHealthy(0)
	c.kickIfOpen()
	assert.Equal(t, int32(1), atomic.LoadInt32(&kicked), "gate open must kick")
}

// TestKickIfOpen_NoKickWhenGateClosed: no kick while the engine reports
// waiting>0 — avoids hold-log spam and pointless dispatch attempts.
func TestKickIfOpen_NoKickWhenGateClosed(t *testing.T) {
	c := newTestAdmission(t, base.KvAdmissionSignal, nil)

	var kicked int32
	c.SetDispatchKick(func() { atomic.AddInt32(&kicked, 1) })

	// Gate closed: engine saturated.
	c.markHealthy(3)
	c.kickIfOpen()
	assert.Equal(t, int32(0), atomic.LoadInt32(&kicked), "gate closed must not kick")

	// Gate closed: degraded (no healthy signals).
	c.mu.Lock()
	c.healthyStreak = 0
	c.mu.Unlock()
	c.kickIfOpen()
	assert.Equal(t, int32(0), atomic.LoadInt32(&kicked), "degraded must not kick")
}
