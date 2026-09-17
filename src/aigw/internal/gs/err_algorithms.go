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

	// CategoryFor resolves a session's locked category; nil falls back to prompt classification.
	CategoryFor func(sessionID string) string
}

// categoryFor: the session's locked category if set, else classify the prompt.
func (a *CatErrAlg) categoryFor(sessionID, prompt string) string {
	if sessionID != "" && a.CategoryFor != nil {
		if cat := a.CategoryFor(sessionID); cat != "" {
			return cat
		}
	}
	if a.Classifier != nil && a.Classifier.IsLoaded() {
		return a.Classifier.Classify(prompt)
	}
	return ""
}

func NewCatErrAlg(classifier *CatErrClassifier, catDists map[string]*EmpiricalDist,
	mu *sync.RWMutex) *CatErrAlg {
	return &CatErrAlg{
		Classifier: classifier,
		CatDists:   catDists,
		mu:         mu,
	}
}

func (a *CatErrAlg) Priority(sessionID string, curStep int, prompt string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if category := a.categoryFor(sessionID, prompt); category != "" {
		a.mu.RLock()
		dist, ok := a.CatDists[category]
		a.mu.RUnlock()
		if ok && dist.TotalCount() > 0 {
			return dist.ExpectedRemaining(curStep)
		}
	}
	if a.mu != nil {
		a.mu.RLock()
		mix, ok := mixtureExpectedRemaining(a.CatDists, curStep)
		a.mu.RUnlock()
		if ok {
			return mix
		}
	}
	return -float64(curStep)
}

func (a *CatErrAlg) PriorityWith(snap *algoSnapshot, sessionID string, curStep int, prompt string) float64 {
	if curStep <= 0 {
		curStep = 1
	}
	if category := a.categoryFor(sessionID, prompt); category != "" {
		if snap != nil && snap.cats != nil {
			if s, ok := snap.cats[category]; ok && s.TotalCount() > 0 {
				return s.ExpectedRemaining(curStep)
			}
		}
	}
	if snap != nil {
		if mix, ok := mixtureExpectedRemaining(snap.cats, curStep); ok {
			return mix
		}
	}
	return -float64(curStep)
}

// mixtureExpectedRemaining ranks an unclassifiable request as a typical
// task: the count-weighted average of per-category E[rem|k].
func mixtureExpectedRemaining[D interface {
	TotalCount() int
	ExpectedRemaining(int) float64
}](cats map[string]D, curStep int) (float64, bool) {
	var weighted float64
	var total int64
	for _, s := range cats {
		n := int64(s.TotalCount())
		if n <= 0 {
			continue
		}
		weighted += s.ExpectedRemaining(curStep) * float64(n)
		total += n
	}
	if total == 0 {
		return 0, false
	}
	return weighted / float64(total), true
}

func (a *CatErrAlg) Name() string { return "caterr" }

// FifoAlg implements FIFO: priority is always 0 so Peek's tie-break
// (EnqueueTime, oldest first) becomes the sole ordering criterion.
type FifoAlg struct{}

func NewFifoAlg() *FifoAlg { return &FifoAlg{} }

func (a *FifoAlg) Priority(_ string, _ int, _ string) float64 { return 0 }

func (a *FifoAlg) PriorityWith(_ *algoSnapshot, _ string, _ int, _ string) float64 { return 0 }

func (a *FifoAlg) Name() string { return "fifo" }
