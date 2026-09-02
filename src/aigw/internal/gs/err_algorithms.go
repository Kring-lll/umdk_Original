/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR scheduling algorithms — MEJ, ERR, CatERR.
 * Create: 2026-08-25
 */

package gs

import (
	"sync"
)

// ErrAlgorithm computes scheduling priority. Lower value = dispatched first.
type ErrAlgorithm interface {
	Priority(sessionID string, curStep int, prompt string) float64
	PriorityWith(snap *algoSnapshot, sessionID string, curStep int, prompt string) float64
	Name() string
}

// MejAlg implements MEJ: priority = -curStep.
type MejAlg struct{}

func NewMejAlg() *MejAlg {
	return &MejAlg{}
}

func (a *MejAlg) Priority(_ string, curStep int, _ string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	return -float64(curStep)
}

func (a *MejAlg) PriorityWith(_ *algoSnapshot, _ string, curStep int, _ string) float64 {
	return a.Priority("", curStep, "")
}

func (a *MejAlg) Name() string { return "mej" }

// ErrAlg implements ERR: priority = dist.ExpectedRemaining(curStep).
type ErrAlg struct {
	Dist *EmpiricalDist
}

func NewErrAlg(dist *EmpiricalDist) *ErrAlg {
	return &ErrAlg{Dist: dist}
}

func (a *ErrAlg) Priority(_ string, curStep int, _ string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if a.Dist == nil {
		return -float64(curStep)
	}
	return a.Dist.ExpectedRemaining(curStep)
}

func (a *ErrAlg) PriorityWith(snap *algoSnapshot, _ string, curStep int, _ string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if snap != nil && snap.global != nil {
		return snap.global.ExpectedRemaining(curStep)
	}
	return a.Priority("", curStep, "")
}

func (a *ErrAlg) Name() string { return "err" }

// CatErrAlg implements CatERR: classify prompt, use per-category E[rem|curStep].
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

func (a *CatErrAlg) Priority(_ string, curStep int, prompt string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if a.Classifier != nil && a.Classifier.IsLoaded() {
		category := a.Classifier.Classify(prompt)
		if category != "" {
			a.mu.RLock()
			dist, ok := a.CatDists[category]
			a.mu.RUnlock()
			if ok && dist.TotalCount() > 0 {
				return dist.ExpectedRemaining(curStep)
			}
		}
	}
	return -float64(curStep)
}

func (a *CatErrAlg) PriorityWith(snap *algoSnapshot, _ string, curStep int, prompt string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if a.Classifier != nil && a.Classifier.IsLoaded() {
		category := a.Classifier.Classify(prompt)
		if category != "" && snap != nil && snap.cats != nil {
			if s, ok := snap.cats[category]; ok && s.TotalCount() > 0 {
				return s.ExpectedRemaining(curStep)
			}
		}
	}
	return -float64(curStep)
}

func (a *CatErrAlg) Name() string { return "caterr" }
