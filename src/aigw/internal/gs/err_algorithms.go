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

// ErrAlgorithm computes scheduling priority. Lower value = dispatched first.
type ErrAlgorithm interface {
	Priority(sessionID string, k int, prompt string) float64
	Name() string
}

// MejAlg implements MEJ: priority = -k. Higher k (closer to completion) = higher priority.
type MejAlg struct{}

func NewMejAlg() *MejAlg {
	return &MejAlg{}
}

func (a *MejAlg) Priority(_ string, k int, _ string) float64 {
	if k <= 0 {
		k = 1
	}
	return -float64(k)
}

func (a *MejAlg) Name() string { return "mej" }

// ErrAlg implements ERR: priority = dist.ExpectedRemaining(k).
type ErrAlg struct {
	Dist *EmpiricalDist
}

func NewErrAlg(dist *EmpiricalDist) *ErrAlg {
	return &ErrAlg{Dist: dist}
}

func (a *ErrAlg) Priority(_ string, k int, _ string) float64 {
	if k <= 0 {
		k = 1
	}
	if a.Dist == nil {
		return -float64(k)
	}
	return a.Dist.ExpectedRemaining(k)
}

func (a *ErrAlg) Name() string { return "err" }

// CatErrAlg implements CatERR: classify prompt, use per-category E[rem|k].
// Falls back to MEJ on classification failure or missing category distribution.
type CatErrAlg struct {
	Classifier *CatErrClassifier
	CatDists   map[string]*EmpiricalDist
	mu         *sync.RWMutex
}

func NewCatErrAlg(classifier *CatErrClassifier, catDists map[string]*EmpiricalDist,
	mu *sync.RWMutex) *CatErrAlg {
	return &CatErrAlg{
		Classifier: classifier,
		CatDists:   catDists,
		mu:         mu,
	}
}

func (a *CatErrAlg) Priority(_ string, k int, prompt string) float64 {
	if k <= 0 {
		k = 1
	}

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
			log.Debug().Msgf("[CatERR] category %s has no dist, falling back to MEJ", category)
			return -float64(k)
		}
	}

	return -float64(k)
}

func (a *CatErrAlg) Name() string { return "caterr" }
