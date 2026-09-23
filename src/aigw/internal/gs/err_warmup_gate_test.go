/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for warmup gating: warmup is all-or-nothing
 * (every enabled gate must pass, all requests rank MEJ until then), CatERR
 * applies both the total gate and the per-category gate, and ErrAlg falls
 * back to MEJ on an empty distribution.
 * Create: 2026-09-21
 */

package gs

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

// TestErrAlgEmptyDistFallsBackToMEJ: an empty distribution must rank MEJ
// (-k), not the constant 0.0 ExpectedReturning returns for empty snapshots —
// with warmupSessions=0 (total gate opted out) the 0.0 would make every
// session rank equal and degrade scheduling to starvation-clock order.
func TestErrAlgEmptyDistFallsBackToMEJ(t *testing.T) {
	alg := NewErrAlg(NewEmpiricalDist(100))
	assert.Equal(t, -5.0, alg.Priority("sess", 5, "prompt"))
	assert.Equal(t, -5.0, alg.PriorityWith(nil, "sess", 5, "prompt"))
}

// TestErrAlgEmptyDistViaManager: the manager-level path with the total gate
// disabled and no observations must also rank MEJ.
func TestErrAlgEmptyDistViaManager(t *testing.T) {
	m := newErrTestGSM(t).errScheduler // warmupSessions=0, empty globalDist
	assert.Equal(t, -3.0, m.Priority("sess", 3, "prompt"))
}

// TestCaterrTotalGateHoldsWarmup: the total gate (sum across categories)
// holds ALL requests at MEJ ranking while unmet, even though every category
// individually has data — and releases when met.
func TestCaterrTotalGateHoldsWarmup(t *testing.T) {
	m := newStickyCatErrScheduler(t) // seed: cluster_0=10, cluster_1=10 (total 20)

	// Total gate above the seeded total: warming, MEJ ranking.
	m.config.WarmupSessions = 21
	assert.True(t, m.IsWarmingUp(), "total 20 < 21 must hold warmup")
	assert.Equal(t, -7.0, m.Priority("sess", 7, stickyOfficePrompt),
		"warmup ranks MEJ even with populated categories")

	// Gate exactly met: warmed, per-category ranking takes over.
	m.config.WarmupSessions = 20
	assert.False(t, m.IsWarmingUp())

	// Total gate with the per-category gate explicitly disabled still applies.
	m.config.WarmupPerCat = -1
	m.config.WarmupSessions = 21
	assert.True(t, m.IsWarmingUp(), "total gate applies even when perCat is disabled")
	m.config.WarmupSessions = 20
	assert.False(t, m.IsWarmingUp())
}

// TestCaterrBothGatesDisabled: negative values disable both gates — the
// scheduler ranks with the algorithm (and its empty-distribution
// fallbacks) from the first request.
func TestCaterrBothGatesDisabled(t *testing.T) {
	m := newStickyCatErrScheduler(t)
	m.config.WarmupSessions = -1
	m.config.WarmupPerCat = -1
	assert.False(t, m.IsWarmingUp(), "both gates disabled must skip warmup entirely")
}

// TestCaterrBothGatesRequired: with both gates enabled, EACH must pass —
// meeting the total gate alone must not exit warmup while a category is
// starved.
func TestCaterrBothGatesRequired(t *testing.T) {
	m := newStickyCatErrScheduler(t) // seed: total 20, each category 10

	m.config.WarmupSessions = 20
	m.config.WarmupPerCat = 15
	assert.True(t, m.IsWarmingUp(), "perCat 10 < 15 must hold warmup despite total met")

	m.config.WarmupPerCat = 10
	assert.False(t, m.IsWarmingUp(), "both gates met must exit warmup")
}
