/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Verify capacity provider and LB schedule() are consistent.
 * Pigeonhole principle: if inFlight < N * batchSize, at least one instance
 * has reqNum < batchSize, so schedule() must find it.
 * Create: 2026-09-03
 */

package gs

import (
	"fmt"
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"huawei.com/aigw/internal/base"
)

// helper: create InstanceManager with N mixed instances, skipInstanceConnection=true
func makeInsManager(t *testing.T, n int, role base.InstanceRole) *InstanceManager {
	t.Helper()
	im := &InstanceManager{
		insPool:                make(map[string]*instance),
		skipInstanceConnection: true,
		poolRWLock:             sync.RWMutex{},
	}
	for i := 0; i < n; i++ {
		url := fmt.Sprintf("http://ins-%d", i)
		ins := &instance{
			insUrl:   url,
			insRole:  role,
			reqNum:   0,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
		}
		im.insPool[url] = ins
	}
	return im
}

// TestCapacityLBConsistency_Mixed verifies: for mixed mode, whenever
// HasCapacity() is true, schedule() must return a non-empty instance.
// When HasCapacity() is false, schedule() must return empty.
func TestCapacityLBConsistency_Mixed(t *testing.T) {
	const N = 3
	const batchSize = 2

	im := makeInsManager(t, N, base.MixedRoleInstance)
	mp := NewInstanceMetricProvider(im)
	lb, err := newRoundRobinLB(mp, &AlgorithmParams{BatchSize: batchSize})
	assert.NoError(t, err)

	cp := NewCapacityProvider(batchSize, func() int { return im.getInsNum() })

	// Phase 1: fill all N*batchSize slots one by one, verify consistency
	for i := 0; i < N*batchSize; i++ {
		assert.True(t, cp.HasCapacity(), "should have capacity at iteration %d (inFlight=%d, slots=%d)",
			i, cp.InFlight(), cp.TotalSlots())

		req := &ScheduleRequestMsg{
			Request: &LlmRequest{ReqId: fmt.Sprintf("req-%d", i), Prompt: "x"},
		}
		result := lb.schedule(req, nil)
		assert.NotEmpty(t, result.PrefillUrl,
			"schedule() returned empty while HasCapacity=true (inFlight=%d, slots=%d)",
			cp.InFlight(), cp.TotalSlots())

		// Simulate dispatchScheduled: AddRequest + OnDispatched
		mp.AddRequest(req.Request, &InstanceContext{InstanceID: result.PrefillUrl})
		cp.OnDispatched()
	}

	// All slots full
	assert.False(t, cp.HasCapacity())
	assert.Equal(t, int32(N*batchSize), cp.InFlight())

	// schedule() should return empty
	req := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "overflow", Prompt: "x"}}
	result := lb.schedule(req, nil)
	assert.Empty(t, result.PrefillUrl, "schedule should return empty when all instances full")

	// Phase 2: complete one request, verify schedule finds an instance
	mp.RemoveRequest(&LlmRequest{ReqId: "req-0"}, &InstanceContext{InstanceID: "http://ins-0"})
	cp.OnCompleted()
	assert.True(t, cp.HasCapacity())
	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "re-dispatch", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
}

// TestCapacityLBConsistency_Asymmetric verifies the invariant holds
// even when all requests pile onto a single instance (non-RR distribution).
func TestCapacityLBConsistency_Asymmetric(t *testing.T) {
	const N = 3
	const batchSize = 5

	im := makeInsManager(t, N, base.MixedRoleInstance)
	mp := NewInstanceMetricProvider(im)
	lb, err := newRoundRobinLB(mp, &AlgorithmParams{BatchSize: batchSize})
	assert.NoError(t, err)
	cp := NewCapacityProvider(batchSize, func() int { return im.getInsNum() })

	// Manually pile all requests onto ins-0
	for i := 0; i < batchSize; i++ {
		reqID := fmt.Sprintf("pile-%d", i)
		mp.AddRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{InstanceID: "http://ins-0"})
		cp.OnDispatched()
	}

	// inFlight = batchSize, TotalSlots = N * batchSize = 15
	// HasCapacity should be true (5 < 15)
	assert.True(t, cp.HasCapacity())
	assert.Equal(t, int32(batchSize), cp.InFlight())

	// ins-0 is full (reqNum=batchSize), but ins-1 and ins-2 are free
	// schedule() should find ins-1 or ins-2
	result := lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "asym", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
	assert.NotEqual(t, "http://ins-0", result.PrefillUrl, "should skip full instance")
}

// TestCapacityLBConsistency_InstanceUnregister verifies that after unregistering
// an instance, HasCapacity() and schedule() remain consistent.
func TestCapacityLBConsistency_InstanceUnregister(t *testing.T) {
	const N = 3
	const batchSize = 2

	im := makeInsManager(t, N, base.MixedRoleInstance)
	mp := NewInstanceMetricProvider(im)
	lb, err := newRoundRobinLB(mp, &AlgorithmParams{BatchSize: batchSize})
	assert.NoError(t, err)
	cp := NewCapacityProvider(batchSize, func() int { return im.getInsNum() })

	// Dispatch 3 requests (fill 1.5 instances worth)
	for i := 0; i < 3; i++ {
		req := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: fmt.Sprintf("r-%d", i), Prompt: "x"}}
		result := lb.schedule(req, nil)
		assert.NotEmpty(t, result.PrefillUrl)
		mp.AddRequest(req.Request, &InstanceContext{InstanceID: result.PrefillUrl})
		cp.OnDispatched()
	}

	// inFlight=3, TotalSlots=6, HasCapacity=true
	assert.True(t, cp.HasCapacity())

	// Unregister ins-2 (which has 1 request from RR)
	im.poolRWLock.Lock()
	delete(im.insPool, "http://ins-2")
	im.poolRWLock.Unlock()
	// inFlight still 3, but TotalSlots = 2 * 2 = 4
	// HasCapacity should still be true (3 < 4)
	assert.True(t, cp.HasCapacity())
	assert.Equal(t, 2, im.getInsNum())

	// schedule() should find an instance among remaining 2
	result := lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "after-unreg", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
	assert.NotEqual(t, "http://ins-2", result.PrefillUrl)
}

// TestCapacityLBConsistency_Separated verifies the pigeonhole principle
// for separated deployment: min(P,D)*batchSize slots, each dispatch
// occupies 1 prefill + 1 decode.
func TestCapacityLBConsistency_Separated(t *testing.T) {
	const P = 2 // prefill instances
	const D = 2 // decode instances
	const batchSize = 2

	im := &InstanceManager{
		insPool:                make(map[string]*instance),
		skipInstanceConnection: true,
		poolRWLock:             sync.RWMutex{},
	}
	for i := 0; i < P; i++ {
		url := fmt.Sprintf("http://prefill-%d", i)
		im.insPool[url] = &instance{
			insUrl:   url,
			insRole:  base.PrefillRoleInstance,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
			groupID:  "group-1",
		}
	}
	for i := 0; i < D; i++ {
		url := fmt.Sprintf("http://decode-%d", i)
		im.insPool[url] = &instance{
			insUrl:   url,
			insRole:  base.DecodeRoleInstance,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
			groupID:  "group-1",
		}
	}

	mp := NewInstanceMetricProvider(im)
	lb, err := newPDLoadBalancer(mp, &AlgorithmParams{
		PdMode:    SeparatedDeployment,
		PrefillLB: LoadBalancerRoundRobin,
		DecodeLB:  LoadBalancerRoundRobin,
		BatchSize: batchSize,
	})
	assert.NoError(t, err)

	cp := NewCapacityProviderSeparated(batchSize,
		func() int { return im.getInsNumByRole(base.PrefillRoleInstance) },
		func() int { return im.getInsNumByRole(base.DecodeRoleInstance) },
	)

	// TotalSlots = min(2,2) * 2 = 4
	assert.Equal(t, 4, cp.TotalSlots())

	// Fill all 4 slots, verify consistency at each step
	for i := 0; i < 4; i++ {
		assert.True(t, cp.HasCapacity(), "step %d: inFlight=%d, slots=%d", i, cp.InFlight(), cp.TotalSlots())

		req := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: fmt.Sprintf("sep-%d", i), Prompt: "x"}}
		result := lb.schedule(req, nil)
		assert.NotEmpty(t, result.PrefillUrl,
			"step %d: schedule returned empty while HasCapacity=true", i)

		mp.AddRequest(req.Request, &InstanceContext{
			InstanceID:       result.PrefillUrl,
			DecodeInstanceID: result.DecodeUrl,
		})
		cp.OnDispatched()
	}

	// Full
	assert.False(t, cp.HasCapacity())

	// schedule() should return empty
	result := lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "overflow", Prompt: "x"}}, nil)
	assert.Empty(t, result.PrefillUrl)

	// Complete one, verify schedule finds instance
	mp.RemoveRequest(&LlmRequest{ReqId: "sep-0"}, &InstanceContext{
		InstanceID:       "http://prefill-0",
		DecodeInstanceID: "http://decode-0",
	})
	cp.OnCompleted()
	assert.True(t, cp.HasCapacity())
	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "re-dispatch", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
}

// TestCapacityLBConsistency_Separated_Asymmetric verifies the case where
// prefill and decode have different counts (min bottleneck).
func TestCapacityLBConsistency_Separated_Asymmetric(t *testing.T) {
	const P = 3
	const D = 1
	const batchSize = 2

	im := &InstanceManager{
		insPool:                make(map[string]*instance),
		skipInstanceConnection: true,
		poolRWLock:             sync.RWMutex{},
	}
	for i := 0; i < P; i++ {
		url := fmt.Sprintf("http://prefill-%d", i)
		im.insPool[url] = &instance{
			insUrl:   url,
			insRole:  base.PrefillRoleInstance,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
			groupID:  "group-1",
		}
	}
	for i := 0; i < D; i++ {
		url := fmt.Sprintf("http://decode-%d", i)
		im.insPool[url] = &instance{
			insUrl:   url,
			insRole:  base.DecodeRoleInstance,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
			groupID:  "group-1",
		}
	}

	mp := NewInstanceMetricProvider(im)
	lb, err := newPDLoadBalancer(mp, &AlgorithmParams{
		PdMode:    SeparatedDeployment,
		PrefillLB: LoadBalancerRoundRobin,
		DecodeLB:  LoadBalancerRoundRobin,
		BatchSize: batchSize,
	})
	assert.NoError(t, err)

	cp := NewCapacityProviderSeparated(batchSize,
		func() int { return im.getInsNumByRole(base.PrefillRoleInstance) },
		func() int { return im.getInsNumByRole(base.DecodeRoleInstance) },
	)

	// TotalSlots = min(3,1) * 2 = 2
	assert.Equal(t, 2, cp.TotalSlots())

	// Fill both slots
	for i := 0; i < 2; i++ {
		assert.True(t, cp.HasCapacity())
		req := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: fmt.Sprintf("asym-%d", i), Prompt: "x"}}
		result := lb.schedule(req, nil)
		assert.NotEmpty(t, result.PrefillUrl)
		mp.AddRequest(req.Request, &InstanceContext{
			InstanceID:       result.PrefillUrl,
			DecodeInstanceID: result.DecodeUrl,
		})
		cp.OnDispatched()
	}

	// Full — decode is the bottleneck (reqNum=2 = batchSize)
	assert.False(t, cp.HasCapacity())
	result := lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "overflow", Prompt: "x"}}, nil)
	assert.Empty(t, result.PrefillUrl)

	// Verify: even though prefill instances have capacity (3*2=6 slots, only 2 used),
	// the decode bottleneck prevents scheduling
	pCount := im.getInsNumByRole(base.PrefillRoleInstance)
	dCount := im.getInsNumByRole(base.DecodeRoleInstance)
	assert.Equal(t, 3, pCount)
	assert.Equal(t, 1, dCount)
}

// TestCapacityLBConsistency_HealthFilter verifies that when an instance
// becomes unhealthy (SSE disconnected), both HasCapacity() and schedule()
// exclude it consistently — no mismatch where ERR dispatches to an
// instance the LB won't route to.
func TestCapacityLBConsistency_HealthFilter(t *testing.T) {
	const N = 2
	const batchSize = 2

	im := &InstanceManager{
		insPool:    make(map[string]*instance),
		poolRWLock: sync.RWMutex{},
	}
	watchers := make(map[string]*sseWatcher)
	for i := 0; i < N; i++ {
		url := fmt.Sprintf("http://health-%d", i)
		ins := &instance{
			insUrl:   url,
			insRole:  base.MixedRoleInstance,
			reqSet:   make(map[string]*LlmRequest),
			reqQueue: newRequestQueue(),
		}
		w := &sseWatcher{enable: true, ins: ins}
		w.Mutex = sync.Mutex{}
		ins.insWatcher = w
		watchers[url] = w
		im.insPool[url] = ins
	}

	mp := NewInstanceMetricProvider(im)
	lb, err := newRoundRobinLB(mp, &AlgorithmParams{BatchSize: batchSize})
	assert.NoError(t, err)
	cp := NewCapacityProvider(batchSize, func() int { return im.getInsNum() })

	// Both healthy: TotalSlots = 2*2 = 4
	assert.Equal(t, 4, cp.TotalSlots())
	assert.Equal(t, 2, im.getInsNum())

	// Dispatch 1 request to health-0 (RR picks first), simulate ERR tracking
	req1 := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: "h-0", Prompt: "x", SessionID: "sess-h-0"}}
	result := lb.schedule(req1, nil)
	assert.NotEmpty(t, result.PrefillUrl)
	mp.AddRequest(req1.Request, &InstanceContext{InstanceID: result.PrefillUrl})
	cp.OnDispatched()

	// Mark health-0 unhealthy
	w0 := watchers["http://health-0"]
	w0.Lock()
	w0.enable = false
	w0.Unlock()

	// After health-0 goes down:
	// - getInsNum() = 1 (only health-1 healthy)
	// - TotalSlots = 1 * 2 = 2
	// - inFlight = 1 (request on health-0 still counted by ERR)
	// - HasCapacity = 1 < 2 = true
	assert.Equal(t, 1, im.getInsNum())
	assert.Equal(t, 2, cp.TotalSlots())
	assert.True(t, cp.HasCapacity(), "should still have capacity (inFlight=1 < TotalSlots=2)")

	// schedule() should only return the healthy instance
	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "h-1", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
	assert.Equal(t, "http://health-1", result.PrefillUrl, "should only route to healthy instance")

	// Simulate ERR cleanup of the stale in-flight request on health-0
	// (in production this happens via onInstanceUnhealthy → errReleaseReq)
	cp.OnCompleted()
	mp.RemoveRequest(&LlmRequest{ReqId: "h-0"}, &InstanceContext{InstanceID: "http://health-0"})

	// After cleanup: inFlight=0, TotalSlots=2 → HasCapacity=true
	assert.Equal(t, int32(0), cp.InFlight())
	assert.True(t, cp.HasCapacity())

	// Fill health-1 to capacity: dispatch h-1 and h-2
	mp.AddRequest(&LlmRequest{ReqId: "h-1"}, &InstanceContext{InstanceID: "http://health-1"})
	cp.OnDispatched()
	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "h-2", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl)
	assert.Equal(t, "http://health-1", result.PrefillUrl)
	mp.AddRequest(&LlmRequest{ReqId: "h-2"}, &InstanceContext{InstanceID: "http://health-1"})
	cp.OnDispatched()

	// Now: inFlight=2, TotalSlots=2 → HasCapacity=false
	assert.False(t, cp.HasCapacity(), "should be full now (inFlight=2, slots=2)")

	// schedule() should return empty (health-1 is full, health-0 excluded)
	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "h-3", Prompt: "x"}}, nil)
	assert.Empty(t, result.PrefillUrl, "should be empty: only healthy instance is full")
}

// TestCapacityLBConsistency_Stress verifies the invariant holds under
// concurrent dispatch/complete operations.
func TestCapacityLBConsistency_Stress(t *testing.T) {
	const N = 5
	const batchSize = 3
	const numGoroutines = 20
	const opsPerGoroutine = 50

	im := makeInsManager(t, N, base.MixedRoleInstance)
	mp := NewInstanceMetricProvider(im)
	lb, err := newRoundRobinLB(mp, &AlgorithmParams{BatchSize: batchSize})
	assert.NoError(t, err)
	cp := NewCapacityProvider(batchSize, func() int { return im.getInsNum() })

	var mu sync.Mutex
	reqCounter := 0
	violations := 0

	var wg sync.WaitGroup
	wg.Add(numGoroutines)

	for g := 0; g < numGoroutines; g++ {
		go func() {
			defer wg.Done()
			for op := 0; op < opsPerGoroutine; op++ {
				mu.Lock()
				// Check invariant: if HasCapacity, schedule must find instance
				if cp.HasCapacity() {
					reqID := fmt.Sprintf("stress-%d", reqCounter)
					reqCounter++
					result := lb.schedule(&ScheduleRequestMsg{
						Request: &LlmRequest{ReqId: reqID, Prompt: "x"},
					}, nil)
					if result.PrefillUrl == "" {
						violations++
					} else {
						mp.AddRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{InstanceID: result.PrefillUrl})
						cp.OnDispatched()
					}
				} else {
					// No capacity: complete a random in-flight request
					if cp.InFlight() > 0 {
						// Find a request to complete
						for url, ins := range im.insPool {
							if ins.reqNum > 0 {
								var reqID string
								for id := range ins.reqSet {
									reqID = id
									break
								}
								if reqID != "" {
									mp.RemoveRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{InstanceID: url})
									cp.OnCompleted()
								}
								break
							}
						}
					}
				}
				mu.Unlock()
			}
		}()
	}

	wg.Wait()
	assert.Equal(t, 0, violations, "pigeonhole principle violated: HasCapacity=true but schedule returned empty")
}

// TestCapacityLBConsistency_GroupIDMismatch verifies that the group-aware
// capacity provider correctly accounts for groupID constraints in separated
// deployment. pdLoadBalancer requires prefill and decode in the SAME group,
// so capacity must be sum over groups of min(prefill_g, decode_g).
//
// Scenario: 2 prefill in group-1, 1 decode in group-1, 1 decode in group-2
//   Old (broken): min(2, 2) * 2 = 4  (counts ALL prefill and ALL decode)
//   Fixed (group-aware): group-1: min(2,1)=1, group-2: min(0,1)=0 → 1 * 2 = 2
func TestCapacityLBConsistency_GroupIDMismatch(t *testing.T) {
	const batchSize = 2

	im := &InstanceManager{
		insPool:                make(map[string]*instance),
		skipInstanceConnection: true,
		poolRWLock:             sync.RWMutex{},
	}

	// 2 prefill in group-1
	im.insPool["http://p1-g1"] = &instance{
		insUrl: "http://p1-g1", insRole: base.PrefillRoleInstance,
		groupID: "group-1", reqSet: make(map[string]*LlmRequest), reqQueue: newRequestQueue(),
	}
	im.insPool["http://p2-g1"] = &instance{
		insUrl: "http://p2-g1", insRole: base.PrefillRoleInstance,
		groupID: "group-1", reqSet: make(map[string]*LlmRequest), reqQueue: newRequestQueue(),
	}
	// 1 decode in group-1
	im.insPool["http://d1-g1"] = &instance{
		insUrl: "http://d1-g1", insRole: base.DecodeRoleInstance,
		groupID: "group-1", reqSet: make(map[string]*LlmRequest), reqQueue: newRequestQueue(),
	}
	// 1 decode in group-2 (no prefill in group-2!)
	im.insPool["http://d2-g2"] = &instance{
		insUrl: "http://d2-g2", insRole: base.DecodeRoleInstance,
		groupID: "group-2", reqSet: make(map[string]*LlmRequest), reqQueue: newRequestQueue(),
	}

	mp := NewInstanceMetricProvider(im)
	lb, err := newPDLoadBalancer(mp, &AlgorithmParams{
		PdMode:    SeparatedDeployment,
		PrefillLB: LoadBalancerRoundRobin,
		DecodeLB:  LoadBalancerRoundRobin,
		BatchSize: batchSize,
	})
	assert.NoError(t, err)

	// Group-aware capacity: group-1: min(2,1)=1, group-2: min(0,1)=0 → total=1
	// TotalSlots = 1 * 2 = 2
	cp := NewCapacityProviderSeparatedGroupAware(batchSize,
		func() int { return im.getSeparatedCapacity() },
	)
	assert.Equal(t, 2, cp.TotalSlots(), "group-aware capacity = 1*2 = 2, not 4")

	// Dispatch 2 requests (fills group-1 decode to batchSize=2)
	for i := 0; i < 2; i++ {
		assert.True(t, cp.HasCapacity(), "step %d: inFlight=%d, slots=%d", i, cp.InFlight(), cp.TotalSlots())
		req := &ScheduleRequestMsg{Request: &LlmRequest{ReqId: fmt.Sprintf("g-%d", i), Prompt: "x"}}
		result := lb.schedule(req, nil)
		assert.NotEmpty(t, result.PrefillUrl, "step %d: should dispatch within group-1", i)
		mp.AddRequest(req.Request, &InstanceContext{
			InstanceID:       result.PrefillUrl,
			DecodeInstanceID: result.DecodeUrl,
		})
		cp.OnDispatched()
	}

	// Now: inFlight=2, TotalSlots=2 → HasCapacity=false (CORRECT!)
	assert.False(t, cp.HasCapacity(), "group-aware capacity correctly reports full")

	// schedule() also returns empty (group-1 decode full, group-2 has no prefill)
	result := lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "g-2", Prompt: "x"}}, nil)
	assert.Empty(t, result.PrefillUrl, "schedule returns empty: no more group-matched capacity")

	// Complete one request → capacity recovers
	mp.RemoveRequest(&LlmRequest{ReqId: "g-0"}, &InstanceContext{
		InstanceID:       "http://p1-g1",
		DecodeInstanceID: "http://d1-g1",
	})
	cp.OnCompleted()
	assert.True(t, cp.HasCapacity(), "capacity recovers after completing one request")

	result = lb.schedule(&ScheduleRequestMsg{Request: &LlmRequest{ReqId: "g-redispatch", Prompt: "x"}}, nil)
	assert.NotEmpty(t, result.PrefillUrl, "should dispatch after completing one request")
}
