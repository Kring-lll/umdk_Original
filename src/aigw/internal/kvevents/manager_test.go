/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: KV events manager enable-merge tests (#900).
 */

package kvevents

import (
	"testing"
	"time"
)

// TestNewKVEventsManager_ConfigEnabledForcesOn: when the prefixCache LB sets
// config.Enabled=true, the manager is active even if AIGW_KV_EVENTS_ENABLED
// is unset. #900: users selecting prefixCache must not also set the env var.
func TestNewKVEventsManager_ConfigEnabledForcesOn(t *testing.T) {
	cfg := KVEventsManagerConfig{
		EndpointTemplate: "tcp://{ip}:5557",
		PollTimeout:      100 * time.Millisecond,
		ReconnectDelay:   time.Second,
		HWM:              1000,
		Enabled:          true,
	}
	m := NewKVEventsManager(countingHandlerAsEventHandler{}, cfg)
	if !m.IsEnabled() {
		t.Fatalf("config.Enabled=true must enable the manager regardless of env")
	}
}

// TestNewKVEventsManager_ConfigDisabledFollowsEnv: when the caller leaves
// config.Enabled=false, IsEnabled() is driven solely by the env var, preserving
// the legacy escape hatch. We cannot reliably toggle the package-level env var
// here (it is read at init), so we only assert the constructor wiring is
// consistent: config.Enabled=false does not force the manager on.
func TestNewKVEventsManager_ConfigDisabledFollowsEnv(t *testing.T) {
	cfg := DefaultKVEventsManagerConfig()
	cfg.Enabled = false
	m := NewKVEventsManager(countingHandlerAsEventHandler{}, cfg)
	// IsEnabled must equal envKVEventsEnabled when config.Enabled is false;
	// we don't assert its exact value (env-dependent), only that the
	// constructor doesn't independently force-enable.
	if m.IsEnabled() != envKVEventsEnabled {
		t.Fatalf("config.Enabled=false must let env drive enablement; got %v", m.IsEnabled())
	}
}

// countingHandlerAsEventHandler is a minimal EventHandler for constructor tests.
type countingHandlerAsEventHandler struct{}

func (countingHandlerAsEventHandler) OnBlockStored(_ BlockStored) error         { return nil }
func (countingHandlerAsEventHandler) OnBlockRemoved(_ BlockRemoved) error       { return nil }
func (countingHandlerAsEventHandler) OnAllBlocksCleared(_ AllBlocksCleared) error { return nil }
