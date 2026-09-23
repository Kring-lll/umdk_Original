/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ErrSchedulerManager test
 * Create: 2026-08-31
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestNewErrSchedulerManagerDisabled(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{SchedulerType: ""})
	if m != nil {
		t.Error("NewErrSchedulerManager with empty type should return nil")
	}

	m = NewErrSchedulerManager(ErrSchedulerConfig{SchedulerType: "none"})
	if m != nil {
		t.Error("NewErrSchedulerManager with 'none' type should return nil")
	}
}

func TestNewErrSchedulerManagerMEJ(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:     5,
		WarmupPerCat:       3,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil for mej")
	}
	if m.AlgorithmName() != "mej (warmup)" {
		t.Errorf("AlgorithmName = %q, want 'mej (warmup)'", m.AlgorithmName())
	}
}

func TestErrSchedulerPriorityWarmup(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	if !m.IsWarmingUp() {
		t.Error("Should be warming up with no observations")
	}
	pri := m.Priority("session-1", 5, "hello")
	if pri != -5 {
		t.Errorf("Priority during warmup curStep=5 = %v, want -5", pri)
	}
}

func TestErrSchedulerOnFinish(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:    2,
		WarmupPerCat:      0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	m.OnRequest("session-1", "model-1", "test prompt")
	m.OnRequest("session-1", "model-1", "test prompt")
	m.OnRequest("session-1", "model-1", "test prompt")
	steps := m.OnFinish("session-1")
	if steps != 3 {
		t.Errorf("OnFinish steps = %d, want 3", steps)
	}

	// MaybeRefresh batches updates (every 10 calls); force refresh for test
	m.globalDist.Refresh()
	if m.globalDist.TotalCount() != 1 {
		t.Errorf("globalDist TotalCount = %d, want 1", m.globalDist.TotalCount())
	}

	// WarmupSessions=2, only 1 observation → still warming up
	if !m.IsWarmingUp() {
		t.Error("Should still be warming up (1 < 2)")
	}

	// Add second session to exit warmup
	m.OnRequest("session-2", "model-1", "test prompt")
	m.OnFinish("session-2")
	m.globalDist.Refresh()
	if m.IsWarmingUp() {
		t.Error("Should not be warming up after 2 observations")
	}
}

func TestErrSchedulerOnCancel(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:    100,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	m.OnRequest("session-1", "model-1", "test prompt")
	m.OnCancel("session-1")

	if m.globalDist.TotalCount() != 0 {
		t.Errorf("globalDist should be empty after cancel, TotalCount = %d", m.globalDist.TotalCount())
	}
}

func TestErrSchedulerAnonymousPriority(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	// During warmup, anonymous priority = -medianK
	pri := m.AnonymousPriority("hello")
	if pri >= 0 {
		t.Errorf("AnonymousPriority during warmup should be negative, got %v", pri)
	}
}

func TestErrSchedulerIsWarmingUpERR(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:    3,
		WarmupPerCat:      0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	if !m.IsWarmingUp() {
		t.Error("Should be warming up initially")
	}

	// Add 3 sessions to exit warmup
	for i := 0; i < 3; i++ {
		sid := "session-" + string(rune('A'+i))
		m.OnRequest(sid, "model-1", "test prompt")
		m.OnFinish(sid)
	}
	m.globalDist.Refresh()
	if m.IsWarmingUp() {
		t.Error("Should not be warming up after 3 observations")
	}
}

func TestErrSchedulerCatERRWarmup(t *testing.T) {
	tmpDir := t.TempDir()

	// Create minimal vocab + centroids
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")
	trainPath := filepath.Join(tmpDir, "train.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world","code"],"GIdf":{"hello":1.5,"world":2.0,"code":1.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0,0.0],[0.0,1.0,1.0]],"labels":["cluster_0","cluster_1"]}`), 0644)
	// Seed satisfies BOTH gates under the all-or-nothing warmup semantics:
	// total 60+40 = 100 >= warmupSessions, each category >= warmupPerCat.
	os.WriteFile(trainPath, []byte(`{"global":{"5":10},"cat_dist":{"cluster_0":{"5":60},"cluster_1":{"3":40}}}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "caterr",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
		TrainDataPath:      trainPath,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil for caterr")
	}

	if !m.classifier.IsLoaded() {
		t.Error("Classifier should be loaded")
	}

	if m.IsWarmingUp() {
		t.Error("Should not be warming up with seeded cat_dist (10 >= 10)")
	}
}

func TestErrSchedulerCatERRWarmupNotEnough(t *testing.T) {
	tmpDir := t.TempDir()

	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world"],"GIdf":{"hello":1.5,"world":2.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0]],"labels":["cluster_0","cluster_1"]}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "caterr",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil")
	}

	if !m.IsWarmingUp() {
		t.Error("Should be warming up with no cat_dist seed")
	}
}

func TestErrSchedulerRestartIdleTimer(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:    100,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	m.OnRequest("session-1", "model-1", "test prompt")
	m.RestartIdleTimer("session-1")
	// Should not panic
}

func TestErrSchedulerAlgorithmNameAfterWarmup(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:    1,
		WarmupPerCat:      0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	if m.AlgorithmName() != "mej (warmup)" {
		t.Errorf("AlgorithmName = %q, want 'mej (warmup)'", m.AlgorithmName())
	}

	m.OnRequest("s1", "model-1", "test prompt")
	m.OnFinish("s1")
	m.globalDist.Refresh()
	if m.AlgorithmName() != "err" {
		t.Errorf("AlgorithmName = %q, want 'err'", m.AlgorithmName())
	}
}

func TestErrSchedulerCompletedCount(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:    100,
		WarmupPerCat:      0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	m.OnRequest("s1", "model-1", "test prompt")
	m.OnFinish("s1")
	if m.CompletedCount() != 1 {
		t.Errorf("CompletedCount = %d, want 1", m.CompletedCount())
	}
}

func TestErrSchedulerUnknownTypeFallsBackToMEJ(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "unknown_type",
		WarmupSessions:    100,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil")
	}
	// During warmup, priority should be MEJ (-curStep)
	pri := m.Priority("s1", 7, "hello")
	if pri != -7 {
		t.Errorf("Priority for unknown type = %v, want -7 (MEJ fallback)", pri)
	}
}

func TestErrSchedulerLoadTrainingDataMismatch(t *testing.T) {
	tmpDir := t.TempDir()

	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")
	trainPath := filepath.Join(tmpDir, "train.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["hello","world"],"GIdf":{"hello":1.5,"world":2.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0]],"labels":["cluster_0","cluster_1"]}`), 0644)
	// cat_dist has labels that don't match classifier labels
	os.WriteFile(trainPath, []byte(`{"global":{"5":10},"cat_dist":{"wrong_cat":{"5":10}}}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "caterr",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
		TrainDataPath:      trainPath,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil")
	}

	// Should be warming up because cat_dist labels don't match classifier labels
	if !m.IsWarmingUp() {
		t.Error("Should be warming up after cat_dist mismatch")
	}
}

func TestErrSchedulerOnFinishZeroSteps(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "mej",
		WarmupSessions:    100,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})
	// OnFinish without OnRequest → steps=0
	steps := m.OnFinish("unknown")
	if steps != 0 {
		t.Errorf("OnFinish for unknown session = %d, want 0", steps)
	}
}

func TestErrSchedulerCatERROnFinishCategory(t *testing.T) {
	tmpDir := t.TempDir()

	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")
	trainPath := filepath.Join(tmpDir, "train.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["code","debug","python","function","sort"],"GIdf":{"code":1.0,"debug":1.5,"python":1.2,"function":1.0,"sort":1.3}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.8,0.9,1.0,0.7],[0.1,0.2,0.3,0.1,0.2]],"labels":["cluster_0","cluster_1"]}`), 0644)
	os.WriteFile(trainPath, []byte(`{"global":{"5":10},"cat_dist":{"cluster_0":{"5":10},"cluster_1":{"3":10}}}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "caterr",
		WarmupSessions:    100,
		WarmupPerCat:      10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
		TrainDataPath:      trainPath,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
	})
	if m == nil {
		t.Fatal("NewErrSchedulerManager returned nil")
	}

	// Simulate a session with code-like prompt
	m.OnRequest("session-1", "model-1", "code debug python function sort")
	m.OnRequest("session-1", "model-1", "code debug python function sort")
	m.OnRequest("session-1", "model-1", "code debug python function sort")
	m.OnFinish("session-1")

	// Category distribution should have been updated
	cat := m.sessionTracker.GetCategory("session-1")
	if cat == "" {
		t.Error("Category should be set for session-1")
	}
}

func TestErrSchedulerAlgoSnapshotERR(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:     0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	// Add some sessions and finish them
	m.OnRequest("s1", "model", "test")
	m.OnFinish("s1")
	m.OnRequest("s2", "model", "test")
	m.OnFinish("s2")
	m.globalDist.Refresh()

	snap := m.AlgoSnapshot()
	assert.NotNil(t, snap)
	assert.NotNil(t, snap.global, "global snapshot should exist for ERR")
	assert.Equal(t, 2, snap.global.TotalCount())
	assert.Nil(t, snap.cats, "cat snapshots should not exist for ERR")
}

func TestErrSchedulerAlgoSnapshotCatERR(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")
	trainPath := filepath.Join(tmpDir, "train.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["code","debug","office","meeting"],"GIdf":{"code":1.0,"debug":1.0,"office":1.0,"meeting":1.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,1.0,0.0,0.0],[0.0,0.0,1.0,1.0]],"labels":["code","office"]}`), 0644)
	os.WriteFile(trainPath, []byte(`{"global":{"5":10,"10":5},"cat_dist":{"code":{"5":10},"office":{"3":5}}}`), 0644)

	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "caterr",
		WarmupSessions:     0,
		WarmupPerCat:       0,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
		TrainDataPath:      trainPath,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	snap := m.AlgoSnapshot()
	assert.NotNil(t, snap)
	assert.Nil(t, snap.global, "global snapshot should not exist for CatERR")
	assert.NotNil(t, snap.cats, "cat snapshots should exist for CatERR")
	assert.Contains(t, snap.cats, "code")
	assert.Contains(t, snap.cats, "office")
	assert.Greater(t, snap.cats["code"].TotalCount(), 0)
	assert.Greater(t, snap.cats["office"].TotalCount(), 0)
}

func TestErrSchedulerAlgoSnapshotConsistency(t *testing.T) {
	m := NewErrSchedulerManager(ErrSchedulerConfig{
		SchedulerType:      "err",
		WarmupSessions:     0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	})

	m.OnRequest("s1", "model", "test")
	m.OnFinish("s1")
	m.globalDist.Refresh()

	// Take snapshot — should be immutable reference
	snap1 := m.AlgoSnapshot()
	count1 := snap1.global.TotalCount()

	// Add more data + refresh
	m.OnRequest("s2", "model", "test")
	m.OnFinish("s2")
	m.globalDist.Refresh()

	// Old snapshot should be unchanged
	assert.Equal(t, count1, snap1.global.TotalCount(), "old snapshot must be immutable")

	// New snapshot should reflect new data
	snap2 := m.AlgoSnapshot()
	assert.Greater(t, snap2.global.TotalCount(), count1, "new snapshot should have more data")
}
