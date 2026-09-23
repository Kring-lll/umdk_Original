/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression test for SDK advisory path skipping ERR accounting.
 * Create: 2026-09-04
 */

package server

import (
	"context"
	"testing"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/core"
	"huawei.com/aigw/internal/gs"
)

// TestSelectOptimalNodeSkipsErrAccounting: regression test.
// SDK callers (aigw_select_nodes) forward the request themselves — aigw
// never observes completion, so the dispatch must not register errInFlight
// (the entry would leak until the zombie scan).
func TestSelectOptimalNodeSkipsErrAccounting(t *testing.T) {
	srv, g := setupErrForwardServer(t, "sdk-advisory-model")
	_ = srv

	mgr := core.NewAigwManagerForTest(&base.AigwConfig{
		Limits: base.Limits{InsNumPerModel: 128, MaxPromptRunes: 100000},
	}, nil, map[string]*gs.GlobalSchedulerManager{"sdk-advisory-model": g})

	out, err := mgr.SelectOptimalNode(&core.GetSuggestionIn{
		UUID:   "sdk-adv-1",
		Model:  "sdk-advisory-model",
		Prompt: "hello",
		Ctx:    context.Background(),
	}, nil)
	if err != nil {
		t.Fatalf("select failed: %v", err)
	}
	if out.TargetPrefillUrl == "" {
		t.Fatalf("expected an instance suggestion")
	}

	if n := g.ErrInFlightCount(); n != 0 {
		t.Fatalf("SDK select_nodes is advisory (aigw never sees completion) — "+
			"must not register in-flight entries, got %d", n)
	}
}
