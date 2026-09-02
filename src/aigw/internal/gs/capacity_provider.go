/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: CapacityProvider — model-level in-flight tracking for ERR scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"sync/atomic"

	"huawei.com/aigw/pkg/log"
)

// CapacityProvider tracks model-level capacity for ERR scheduling.
// separated deployment uses min(prefill, decode) since each request
// occupies one slot in both pools.
type CapacityProvider struct {
	batchSize      int
	instanceCount  func() int
	prefillCount   func() int
	decodeCount    func() int
	inFlight       atomic.Int32
}

func NewCapacityProvider(batchSize int, instanceCount func() int) *CapacityProvider {
	if instanceCount == nil {
		instanceCount = func() int { return 0 }
	}
	return &CapacityProvider{
		batchSize:     batchSize,
		instanceCount: instanceCount,
	}
}

func NewCapacityProviderSeparated(batchSize int, prefillCount func() int, decodeCount func() int) *CapacityProvider {
	if prefillCount == nil {
		prefillCount = func() int { return 0 }
	}
	if decodeCount == nil {
		decodeCount = func() int { return 0 }
	}
	return &CapacityProvider{
		batchSize:    batchSize,
		prefillCount: prefillCount,
		decodeCount:  decodeCount,
	}
}

func (c *CapacityProvider) TotalSlots() int {
	if c == nil {
		return 0
	}
	if c.prefillCount != nil && c.decodeCount != nil {
		p := c.prefillCount()
		d := c.decodeCount()
		if p < d {
			return p * c.batchSize
		}
		return d * c.batchSize
	}
	return c.instanceCount() * c.batchSize
}

func (c *CapacityProvider) HasCapacity() bool {
	if c == nil {
		return false
	}
	return c.inFlight.Load() < int32(c.TotalSlots())
}

func (c *CapacityProvider) FreeSlots() int {
	if c == nil {
		return 0
	}
	return c.TotalSlots() - int(c.inFlight.Load())
}

func (c *CapacityProvider) OnDispatched() {
	if c == nil {
		return
	}
	c.inFlight.Add(1)
}

func (c *CapacityProvider) InFlight() int32 {
	if c == nil {
		return 0
	}
	return c.inFlight.Load()
}

// OnCompleted decrements the in-flight counter. Never goes below 0.
func (c *CapacityProvider) OnCompleted() {
	if c == nil {
		return
	}
	for {
		cur := c.inFlight.Load()
		if cur <= 0 {
			log.Warn().Msgf("[Capacity] OnCompleted no-op: inFlight already 0")
			return
		}
		if c.inFlight.CompareAndSwap(cur, cur-1) {
			return
		}
	}
}

func (c *CapacityProvider) InstanceCount() int {
	if c == nil {
		return 0
	}
	if c.prefillCount != nil && c.decodeCount != nil {
		p := c.prefillCount()
		d := c.decodeCount()
		if p < d {
			return p
		}
		return d
	}
	if c.instanceCount == nil {
		return 0
	}
	return c.instanceCount()
}

func (c *CapacityProvider) BatchSize() int {
	if c == nil {
		return 0
	}
	return c.batchSize
}
