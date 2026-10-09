/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: CapacityProvider test
 * Create: 2026-09-02
 */

package gs

import (
	"sync/atomic"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestCapacityProviderMixed(t *testing.T) {
	c := NewCapacityProvider(2, func() int { return 3 })
	assert.Equal(t, 6, c.TotalSlots())
	assert.True(t, c.HasCapacity())
	assert.Equal(t, 6, c.FreeSlots())

	c.OnDispatched()
	c.OnDispatched()
	assert.Equal(t, int32(2), c.InFlight())
	assert.Equal(t, 4, c.FreeSlots())
	assert.True(t, c.HasCapacity())

	// Fill all slots
	c.OnDispatched()
	c.OnDispatched()
	c.OnDispatched()
	c.OnDispatched()
	assert.False(t, c.HasCapacity())
	assert.Equal(t, 0, c.FreeSlots())

	// Over-subscribe
	c.OnDispatched()
	assert.False(t, c.HasCapacity())
	assert.Equal(t, -1, c.FreeSlots())

	// Release
	c.OnCompleted()
	assert.Equal(t, int32(6), c.InFlight())
	assert.Equal(t, 0, c.FreeSlots())
}

func TestCapacityProviderNilSafe(t *testing.T) {
	var c *CapacityProvider
	assert.Equal(t, 0, c.TotalSlots())
	assert.False(t, c.HasCapacity())
	assert.Equal(t, 0, c.FreeSlots())
	assert.Equal(t, int32(0), c.InFlight())
	c.OnDispatched()   // no panic
	c.OnCompleted()    // no panic
	assert.Equal(t, 0, c.InstanceCount())
	assert.Equal(t, 0, c.BatchSize())
}

func TestCapacityProviderOnCompletedFloorZero(t *testing.T) {
	c := NewCapacityProvider(1, func() int { return 1 })

	// Decrement without prior dispatch — should floor at 0, not go negative
	c.OnCompleted()
	assert.Equal(t, int32(0), c.InFlight())

	c.OnDispatched()
	assert.Equal(t, int32(1), c.InFlight())
	c.OnCompleted()
	assert.Equal(t, int32(0), c.InFlight())
}

func TestCapacityProviderSeparated(t *testing.T) {
	var prefill atomic.Int32
	var decode atomic.Int32
	prefill.Store(3)
	decode.Store(1)

	c := NewCapacityProviderSeparated(2, func() int { return int(prefill.Load()) }, func() int { return int(decode.Load()) })

	// min(3, 1) * 2 = 2
	assert.Equal(t, 2, c.TotalSlots())
	assert.Equal(t, 1, c.InstanceCount()) // min(3, 1) = 1
	assert.True(t, c.HasCapacity())

	// Fill 2 slots
	c.OnDispatched()
	c.OnDispatched()
	assert.False(t, c.HasCapacity())
	assert.Equal(t, 0, c.FreeSlots())

	// Add decode instance: min(3, 2) * 2 = 4
	decode.Store(2)
	assert.Equal(t, 4, c.TotalSlots())
	assert.True(t, c.HasCapacity())
	assert.Equal(t, 2, c.FreeSlots())

	// Remove all decode: min(3, 0) * 2 = 0
	decode.Store(0)
	assert.Equal(t, 0, c.TotalSlots())
	assert.False(t, c.HasCapacity())
}

func TestCapacityProviderSeparatedNilCallbacks(t *testing.T) {
	c := NewCapacityProviderSeparated(2, nil, nil)
	assert.Equal(t, 0, c.TotalSlots())
	assert.Equal(t, 0, c.InstanceCount())
	assert.False(t, c.HasCapacity())
}

func TestCapacityProviderConcurrentOnCompleted(t *testing.T) {
	c := NewCapacityProvider(1, func() int { return 10 })

	// Dispatch 100, then concurrently complete all
	for i := 0; i < 100; i++ {
		c.OnDispatched()
	}
	assert.Equal(t, int32(100), c.InFlight())

	done := make(chan struct{}, 100)
	for i := 0; i < 100; i++ {
		go func() {
			c.OnCompleted()
			done <- struct{}{}
		}()
	}
	for i := 0; i < 100; i++ {
		<-done
	}
	// Should floor at 0, not negative
	assert.Equal(t, int32(0), c.InFlight())
}
