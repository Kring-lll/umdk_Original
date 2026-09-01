/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: KV events manager construction tests (#900).
 */

package kvevents

import (
	"testing"
	"time"
)

// TestNewKVEventsManager_ConstructedActive: manager is active on construction.
func TestNewKVEventsManager_ConstructedActive(t *testing.T) {
	cfg := KVEventsManagerConfig{
		EndpointTemplate: "tcp://{ip}:5557",
		PollTimeout:      100 * time.Millisecond,
		ReconnectDelay:   time.Second,
		HWM:              1000,
	}
	m := NewKVEventsManager(countingHandlerAsEventHandler{}, cfg)
	if !m.IsEnabled() {
		t.Fatalf("constructed manager must be active")
	}
}

// countingHandlerAsEventHandler is a minimal EventHandler for constructor tests.
type countingHandlerAsEventHandler struct{}

func (countingHandlerAsEventHandler) OnBlockStored(_ BlockStored) error         { return nil }
func (countingHandlerAsEventHandler) OnBlockRemoved(_ BlockRemoved) error       { return nil }
func (countingHandlerAsEventHandler) OnAllBlocksCleared(_ AllBlocksCleared) error { return nil }
