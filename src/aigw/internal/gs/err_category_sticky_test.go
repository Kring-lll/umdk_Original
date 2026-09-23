/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: regression tests for CatERR session-category stickiness,
 * sub-agent inheritance, and the zero-vector MEJ fallback — the two v2/v7
 * misclassification mechanisms (delegation-text domain mismatch,
 * compaction rewrite) both stem from per-request classification with no
 * session category memory.
 * Create: 2026-09-09
 */

package gs

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
)

// newStickyCatErrScheduler builds a caterr manager whose classifier separates
// office vocabulary (hello/calling/account) from code vocabulary
// (files/workspace/report), with deliberately distinct per-cluster
// remaining-step expectations: cluster_0 E[rem|1]=79 (worst tier),
// cluster_1 E[rem|1]=2 (protected tier).
func newStickyCatErrScheduler(t *testing.T) *ErrSchedulerManager {
	t.Helper()
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")
	trainPath := filepath.Join(tmpDir, "train.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","calling","account","files","workspace","report"],
		"GIdf":{"hello":1.5,"calling":1.5,"account":1.5,"files":1.5,"workspace":1.5,"report":1.5}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[0,0,0,1,1,1],[1,1,1,0,0,0]],
		"labels":["cluster_0","cluster_1"]}`), 0644)
	os.WriteFile(trainPath, []byte(`{"global":{"5":10},"cat_dist":{"cluster_0":{"80":10},"cluster_1":{"3":10}}}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType: "caterr",
		// Total gate explicitly disabled: the seed (10+10 samples) is below
		// the default total threshold, and these tests exercise post-warmup
		// algorithm ranking with the per-category gate met by the seed.
		WarmupSessions:     -1,
		WarmupPerCat:       10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
		TrainDataPath:      trainPath,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil for caterr")
	}
	return m
}

const (
	stickyOfficePrompt = "hello calling about my account billing question"
	stickyCodePrompt   = "search the workspace files and report file paths"
)

// TestSessionCategorySticky: the session's category is decided by its first
// classifiable request and never re-examined. Mid-session input-form changes
// (compaction rewrites the history into an engineering-checkpoint summary,
// v7 tau2-42: cluster_1→cluster_0 flip) must not change the priority tier.
func TestSessionCategorySticky(t *testing.T) {
	m := newStickyCatErrScheduler(t)

	// First request: office vocabulary → cluster_1.
	m.OnRequest("sess-sticky", "model", stickyOfficePrompt)
	assert.Equal(t, "cluster_1", m.SessionCategory("sess-sticky"))

	// Later request: the same session's input now reads as code (e.g. a
	// compaction summary). The category must NOT flip.
	m.OnRequest("sess-sticky", "model", stickyCodePrompt)
	assert.Equal(t, "cluster_1", m.SessionCategory("sess-sticky"),
		"category is locked at the first classifiable request")

	snap := m.AlgoSnapshot()
	pri := m.PriorityWith(snap, "sess-sticky", 1, stickyCodePrompt)
	assert.InDelta(t, 2.0, pri, 0.001,
		"priority must come from the locked cluster_1 distribution (E[rem|1]=2), not the flipped cluster_0 (79)")
}

// TestSubSessionInheritsParentCategory: an SDK sub-agent session
// ("{parent}_sub_{type}_{uuid}") inherits the parent's category instead of
// classifying its delegation text — a delegation written for a
// "codebase navigation specialist" lives in code vocabulary regardless of
// the parent task's domain (v2 office-44: office task's explore_agent
// request classified cluster_0, priority 75.86, starved 290s to death).
func TestSubSessionInheritsParentCategory(t *testing.T) {
	m := newStickyCatErrScheduler(t)

	// Parent session locks cluster_1 (office).
	m.OnRequest("pz-office-44", "model", stickyOfficePrompt)
	assert.Equal(t, "cluster_1", m.SessionCategory("pz-office-44"))

	// The sub-agent's first request carries code-domain delegation text.
	sub := "pz-office-44_sub_explore_agent_7870a3dd"
	m.OnRequest(sub, "model", stickyCodePrompt)
	assert.Equal(t, "cluster_1", m.SessionCategory(sub),
		"sub-agent session must inherit the parent's category, not classify its delegation text")

	snap := m.AlgoSnapshot()
	pri := m.PriorityWith(snap, sub, 1, stickyCodePrompt)
	assert.InDelta(t, 2.0, pri, 0.001,
		"the sub-agent must run at the parent's tier, not the code-worst tier (79)")
}

// TestUnclassifiablePromptRunsAtMixturePriority: text with zero vocabulary
// hit (any Chinese input against the all-English vocab) must not be asserted
// as the first cluster — the session stays unlocked (a later classifiable
// request may lock it) and the request ranks at the count-weighted mixture
// of per-category E[rem|k]: a negative -k fallback would outrank EVERY
// classified session (all positive E[rem]), the old labels[0] fallback
// parked it in the worst tier. With equal-mass test clusters the mixture at
// k=1 is (79 + 2) / 2 = 40.5.
func TestUnclassifiablePromptRunsAtMixturePriority(t *testing.T) {
	m := newStickyCatErrScheduler(t)

	assert.Equal(t, "", m.classifier.Classify("在工作区中搜索是否已有工时记录"),
		"zero-hit input must classify as \"\" (was silently asserted cluster_0)")

	m.OnRequest("sess-cn", "model", "在工作区中搜索是否已有工时记录")
	assert.Equal(t, "", m.SessionCategory("sess-cn"),
		"an unclassifiable request must lock nothing")

	snap := m.AlgoSnapshot()
	pri := m.PriorityWith(snap, "sess-cn", 1, "在工作区中搜索是否已有工时记录")
	assert.InDelta(t, 40.5, pri, 0.001,
		"unclassifiable ranks at the mixture (79+2)/2, not -k (would outrank everything) nor cluster_0 (worst tier)")

	// The advisory hint (get-suggestion / SDK select_nodes, always session-less)
	// still answers with the typical-task mixture through the pooled-median
	// step (both categories median 80/3, equal mass → step 41 → mixture
	// E[rem|41] = (39+0)/2 = 19.5). ERR dispatch itself ranks anonymous
	// requests at the boost tier (MinInt32), covered in err_starvation_test.
	anon := m.AnonymousPriority("在工作区中搜索是否已有工时记录")
	assert.InDelta(t, 19.5, anon, 0.001,
		"advisory hint for unclassifiable traffic ranks mid-tier via pooled median + mixture")

	// A later classifiable request may still lock the session.
	m.OnRequest("sess-cn", "model", stickyOfficePrompt)
	assert.Equal(t, "cluster_1", m.SessionCategory("sess-cn"))
}

// TestSubAgentInheritsParentIteration: sub-agents run synchronously inside
// the parent's tool call — their progress IS the parent's progress. A
// sub-agent created while the parent is at k=7 must start at k=7 (not 1),
// so SJF ranks it with the parent instead of as a fresh longest-remaining
// task.
func TestSubAgentInheritsParentIteration(t *testing.T) {
	m := newMejTestGSM(t)

	// Parent reaches k=7.
	for i := 0; i < 7; i++ {
		m.errScheduler.OnRequest("parent-1", "model", "p")
	}
	assert.Equal(t, 7, m.errScheduler.SessionIteration("parent-1"))

	// Sub-agent session's first request inherits k=7.
	sub := "parent-1_sub_explore_agent_a58c7568"
	assert.Equal(t, 7, m.errScheduler.OnRequest(sub, "model", "delegation text"))
	assert.Equal(t, 7, m.errScheduler.SessionIteration(sub))

	// Sub-agent progresses independently afterwards.
	assert.Equal(t, 8, m.errScheduler.OnRequest(sub, "model", "p2"))
	assert.Equal(t, 8, m.errScheduler.SessionIteration(sub))
	// Parent untouched.
	assert.Equal(t, 7, m.errScheduler.SessionIteration("parent-1"))

	// MEJ priority: sub-agent at inherited k ranks with the parent, not at -1.
	snap := m.errScheduler.AlgoSnapshot()
	priSub := m.errScheduler.PriorityWith(snap, sub, 7, "p")
	assert.InDelta(t, -7.0, priSub, 0.001,
		"sub-agent at inherited k=7 must rank at -7 (with the parent), not -1 (fresh)")
}

// TestSubAgentParentGoneStartsAtOne: parent session expired or absent —
// the sub-agent falls back to k=1 (no crash, no phantom inheritance).
func TestSubAgentParentGoneStartsAtOne(t *testing.T) {
	m := newErrTestGSM(t)
	sub := "ghost-parent_sub_plan_agent_0000"
	assert.Equal(t, 1, m.errScheduler.OnRequest(sub, "model", "p"))
	assert.Equal(t, 1, m.errScheduler.SessionIteration(sub))
}

// TestNormalSessionUnaffectedByInheritance: non-sub sessions start at 1
// as before.
func TestNormalSessionUnaffectedByInheritance(t *testing.T) {
	m := newErrTestGSM(t)
	assert.Equal(t, 1, m.errScheduler.OnRequest("plain-session", "model", "p"))
	assert.Equal(t, 2, m.errScheduler.OnRequest("plain-session", "model", "p"))
	assert.Equal(t, 1, m.errScheduler.OnRequest("plain-session-sub", "model", "p"),
		"'-sub' suffix without _sub_ marker is not a sub-agent session")
}
