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
// batchSize is the per-instance max concurrency.
type CapacityProvider struct {
	mu        sync.RWMutex
	batchSize int
	inFlight  map[string]*int32
}

const defaultBatchSize = 1

func NewCapacityProvider(batchSize int) *CapacityProvider {
	if batchSize <= 0 {
		batchSize = defaultBatchSize
	}
	return &CapacityProvider{
		batchSize: batchSize,
		inFlight:  make(map[string]*int32),
	}
}

func (c *CapacityProvider) getOrCreateCounter(insURL string) *int32 {
	c.mu.RLock()
	counter, ok := c.inFlight[insURL]
	c.mu.RUnlock()
	if ok {
		return counter
	}

	c.mu.Lock()
	defer c.mu.Unlock()
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
// Never drives the counter below 0: a spurious duplicate completion is a no-op.
func (c *CapacityProvider) OnCompleted(insURL string) {
	counter := c.getOrCreateCounter(insURL)
	for {
		cur := atomic.LoadInt32(counter)
		if cur <= 0 {
			log.Warn().Msgf("[Capacity] OnCompleted no-op on %s: inFlight already 0", insURL)
			return
		}
		if atomic.CompareAndSwapInt32(counter, cur, cur-1) {
			log.Debug().Msgf("[Capacity] completed on %s, inFlight=%d/%d", insURL, cur-1, c.batchSize)
			return
		}
	}
}

