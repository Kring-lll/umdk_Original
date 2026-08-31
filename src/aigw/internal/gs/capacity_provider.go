/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: CapacityProvider — tracks per-instance in-flight request count.
 * Create: 2026-08-25
 */

package gs

import (
	"sync"
	"sync/atomic"

	"huawei.com/aigw/pkg/log"
)

// CapacityProvider tracks instance capacity for ERR scheduling.
// In the integration phase, we use batchSize as the per-instance max concurrency.
// In the production version, this will be replaced by pymotor callback API.
type CapacityProvider struct {
	mu        sync.RWMutex
	batchSize int
	inFlight  map[string]*int32 // key = instance URL → atomic counter
}

// NewCapacityProvider creates a new CapacityProvider.
// batchSize is the max concurrent requests per instance.
func NewCapacityProvider(batchSize int) *CapacityProvider {
	if batchSize <= 0 {
		batchSize = 1
	}
	return &CapacityProvider{
		batchSize: batchSize,
		inFlight:  make(map[string]*int32),
	}
}

// getOrCreateCounter returns the atomic counter for an instance, creating if needed.
func (c *CapacityProvider) getOrCreateCounter(insURL string) *int32 {
	c.mu.RLock()
	counter, ok := c.inFlight[insURL]
	c.mu.RUnlock()
	if ok {
		return counter
	}

	c.mu.Lock()
	defer c.mu.Unlock()
	// Double-check after acquiring write lock
	if counter, ok = c.inFlight[insURL]; ok {
		return counter
	}
	var v int32
	counter = &v
	c.inFlight[insURL] = counter
	return counter
}

// HasFreeSlot returns true if the instance has capacity for another request.
func (c *CapacityProvider) HasFreeSlot(insURL string) bool {
	counter := c.getOrCreateCounter(insURL)
	current := atomic.LoadInt32(counter)
	return int(current) < c.batchSize
}

// OnDispatched is called when a request is dispatched to an instance.
func (c *CapacityProvider) OnDispatched(insURL string) {
	counter := c.getOrCreateCounter(insURL)
	newVal := atomic.AddInt32(counter, 1)
	log.Debug().Msgf("[Capacity] dispatched to %s, inFlight=%d/%d", insURL, newVal, c.batchSize)
}

// OnCompleted is called when a request completes on an instance.
//
// Never drives the in-flight counter below 0: the decrement is performed only
// while the counter is still positive, so a spurious or duplicate completion
// (e.g. a second OnCompleted for an already-freed slot) is a no-op instead of
// creating a negative count. A negative count would under-count in-flight
// requests and cause over-subscription -- exactly the risk reviewer [8] raised.
// This is strictly safer than "decrement then reset-to-0": it can only ever
// under-count (conservative -> never over-subscribe), never clobber a concurrent
// OnDispatched's legitimate positive value.
func (c *CapacityProvider) OnCompleted(insURL string) {
	counter := c.getOrCreateCounter(insURL)
	for {
		cur := atomic.LoadInt32(counter)
		if cur <= 0 {
			// Already 0 (duplicate/spurious completion): don't decrement. A
			// duplicate reaching here is unexpected in the current http_server
			// flow (ErrOnCancel returns before ErrOnResponse), so log it.
			log.Warn().Msgf("[Capacity] OnCompleted no-op on %s: inFlight already 0 (duplicate completion?)", insURL)
			return
		}
		if atomic.CompareAndSwapInt32(counter, cur, cur-1) {
			log.Debug().Msgf("[Capacity] completed on %s, inFlight=%d/%d", insURL, cur-1, c.batchSize)
			return
		}
	}
}

// GetInFlight returns the current in-flight count for an instance.
func (c *CapacityProvider) GetInFlight(insURL string) int {
	counter := c.getOrCreateCounter(insURL)
	return int(atomic.LoadInt32(counter))
}
