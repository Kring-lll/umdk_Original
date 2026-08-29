/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR scheduling algorithms — MEJ, ERR, CatERR.
 * Create: 2026-08-25
 */

package gs

import (
	"sync"

	"huawei.com/aigw/pkg/log"
)

// ErrAlgorithm is the interface for ERR scheduling priority computation.
// Lower priority value = higher scheduling priority (dispatched first).
type ErrAlgorithm interface {
	// Priority returns the scheduling priority for a session at iteration k.
	// Lower values are dispatched first.
	Priority(sessionID string, k int, prompt string) float64

	// Name returns the algorithm name for logging.
	Name() string
}

// ─── MEJ (Most Executed Job) ────────────────────────────────────────────

// MejAlg implements MEJ: priority = maxK + 1 - k.
// k越大越优先 (越接近完成). maxK+1-k 转为正数避免负值兼容性问题.
type MejAlg struct {
	MaxK int
}

// NewMejAlg creates a MEJ algorithm with the given maxK offset.
func NewMejAlg(maxK int) *MejAlg {
	if maxK <= 0 {
		maxK = 10000
	}
	return &MejAlg{MaxK: maxK}
}

func (a *MejAlg) Priority(_ string, k int, _ string) float64 {
	if k <= 0 {
		k = 1
	}
	return float64(a.MaxK + 1 - k)
}

func (a *MejAlg) Name() string { return "mej" }

// ─── ERR (Expected Remaining Rounds) ────────────────────────────────────

// ErrAlg implements ERR: priority = dist.ExpectedRemaining(k).
// Uses a single global EmpiricalDist (per-model).
type ErrAlg struct {
	Dist *EmpiricalDist
}

// NewErrAlg creates an ERR algorithm with the given distribution.
func NewErrAlg(dist *EmpiricalDist) *ErrAlg {
	return &ErrAlg{Dist: dist}
}

func (a *ErrAlg) Priority(_ string, k int, _ string) float64 {
	if k <= 0 {
		k = 1
	}
	if a.Dist == nil {
		// No distribution available — degenerate to MEJ (k越大越优先)
		return float64(10001 - k)
	}
	return a.Dist.ExpectedRemaining(k)
}

func (a *ErrAlg) Name() string { return "err" }

// ─── CatERR (Categorized ERR) ───────────────────────────────────────────

// CatErrAlg implements CatERR: classify prompt → category → use that
// category's EmpiricalDist for E[rem|k]. Falls back to global dist if
// classification fails or category has no distribution.
type CatErrAlg struct {
	Classifier *CatErrClassifier
	CatDists   map[string]*EmpiricalDist // per-category distributions
	GlobalDist *EmpiricalDist           // fallback distribution
	mu         *sync.RWMutex             // protects CatDists map (runtime writes in OnFinish), see review [12]
}

// NewCatErrAlg creates a CatERR algorithm. mu protects the CatDists map against
// runtime writes from OnFinish (new categories created at runtime); pass the
// ErrSchedulerManager's shared mutex so reads and writes agree.
func NewCatErrAlg(classifier *CatErrClassifier, catDists map[string]*EmpiricalDist,
	globalDist *EmpiricalDist, mu *sync.RWMutex) *CatErrAlg {
	return &CatErrAlg{
		Classifier: classifier,
		CatDists:   catDists,
		GlobalDist: globalDist,
		mu:         mu,
	}
}

func (a *CatErrAlg) Priority(_ string, k int, prompt string) float64 {
	if k <= 0 {
		k = 1
	}

	// Try category-specific distribution
	if a.Classifier != nil && a.Classifier.IsLoaded() {
		category := a.Classifier.Classify(prompt)
		if category != "" {
			if a.mu != nil {
				a.mu.RLock()
			}
			dist, ok := a.CatDists[category]
			if a.mu != nil {
				a.mu.RUnlock()
			}
			if ok && dist.TotalCount() > 0 {
				return dist.ExpectedRemaining(k)
			}
			// Category has no per-category distribution. IsWarmingUp should
			// have caught this and returned MEJ. As a defensive fallback
			// (e.g. new category after recluster), degrade to MEJ — NOT
			// global E[rem] (global mixes all categories, no discrimination).
			log.Debug().Msgf("[CatERR] category %s has no dist, falling back to MEJ", category)
			return float64(10001 - k)
		}
	}

	// Classification failed — degenerate to MEJ.
	// (CatERR does not use globalDist for priority.)
	return float64(10001 - k)
}

func (a *CatErrAlg) Name() string { return "caterr" }
