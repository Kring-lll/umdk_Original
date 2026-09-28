/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: PendingQueue — per-model priority queue for ERR scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"errors"
	"fmt"
	"math"
	"sync"
	"time"
)

// PendingRequest represents a request held in the pending queue.
type PendingRequest struct {
	ReqID       string
	SessionID   string
	CurStep     int
	Msg         *ControlMessage
	EnqueueTime time.Time
}

// PendingQueue is a per-model pending queue using a hash map.
// The map key is ReqID (unique per request), NOT SessionID.
type PendingQueue struct {
	mu       sync.Mutex
	maxSize  int
	requests map[string]*PendingRequest
}

func NewPendingQueue(maxSize int) *PendingQueue {
	return &PendingQueue{
		maxSize:  maxSize,
		requests: make(map[string]*PendingRequest),
	}
}

var ErrQueueFull = errors.New("pending queue full")
var ErrNoInstance = errors.New("no instance available")
var ErrDuplicateReqID = errors.New("duplicate request ID in pending queue")

func (q *PendingQueue) Add(req *PendingRequest) error {
	if req == nil {
		return nil
	}
	q.mu.Lock()
	defer q.mu.Unlock()

	if req.ReqID == "" {
		req.ReqID = fmt.Sprintf("auto-%p", req)
	}
	if req.EnqueueTime.IsZero() {
		req.EnqueueTime = time.Now()
	}
	if _, exists := q.requests[req.ReqID]; exists {
		return ErrDuplicateReqID
	}
	if q.maxSize > 0 && len(q.requests) >= q.maxSize {
		return ErrQueueFull
	}
	q.requests[req.ReqID] = req
	return nil
}

// Peek returns the highest-priority request without removing it from the
// queue. Priority is computed on-the-fly by fn. Ties broken by EnqueueTime
// (oldest first). Call Remove(reqID) only after the request is dispatched.
func (q *PendingQueue) Peek(fn func(*PendingRequest) int) *PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	if len(q.requests) == 0 {
		return nil
	}

	var best *PendingRequest
	bestPri := math.MaxInt32

	for _, req := range q.requests {
		pri := fn(req)
		if best == nil || pri < bestPri ||
			(pri == bestPri && req.EnqueueTime.Before(best.EnqueueTime)) {
			bestPri = pri
			best = req
		}
	}
	return best
}

func (q *PendingQueue) Remove(reqID string) {
	q.mu.Lock()
	defer q.mu.Unlock()
	delete(q.requests, reqID)
}

func (q *PendingQueue) IsEmpty() bool {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests) == 0
}

func (q *PendingQueue) IsFull() bool {
	if q.maxSize == 0 {
		return false
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests) >= q.maxSize
}

func (q *PendingQueue) Size() int {
	if q == nil {
		return 0
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests)
}

// RemoveBySession removes all queued requests of the session and returns
// them. Callers must write a terminal error to each removed request's
// Response channel so its handler does not hang until client timeout.
func (q *PendingQueue) RemoveBySession(sessionID string) []*PendingRequest {
	if sessionID == "" {
		return nil
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	var removed []*PendingRequest
	for id, req := range q.requests {
		if req.SessionID == sessionID {
			delete(q.requests, id)
			removed = append(removed, req)
		}
	}
	return removed
}

// PeekLowestStep returns the request with the smallest CurStep without removing it.
// Returns nil if the queue is empty.
func (q *PendingQueue) PeekLowestStep() *PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	if len(q.requests) == 0 {
		return nil
	}

	var victim *PendingRequest
	for _, req := range q.requests {
		if victim == nil || req.CurStep < victim.CurStep {
			victim = req
			if req.CurStep <= 1 {
				break
			}
		} else if req.CurStep == victim.CurStep && req.EnqueueTime.After(victim.EnqueueTime) {
			victim = req
		}
	}
	return victim
}

// EvictLowestStep finds and removes the request with the smallest CurStep (least progress).
// Among same-CurStep requests, picks the one with the shortest wait (most recent EnqueueTime).
// Returns the evicted request, or nil if the queue is empty.
func (q *PendingQueue) EvictLowestStep() *PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	if len(q.requests) == 0 {
		return nil
	}

	var victim *PendingRequest
	for _, req := range q.requests {
		if victim == nil {
			victim = req
			continue
		}
		if req.CurStep < victim.CurStep {
			victim = req
			if req.CurStep <= 1 {
				break
			}
		} else if req.CurStep == victim.CurStep && req.EnqueueTime.After(victim.EnqueueTime) {
			victim = req
		}
	}

	if victim != nil {
		delete(q.requests, victim.ReqID)
	}
	return victim
}

// DrainAll removes and returns all pending requests (for instance-unregister cleanup).
func (q *PendingQueue) DrainAll() []*PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	result := make([]*PendingRequest, 0, len(q.requests))
	for _, req := range q.requests {
		result = append(result, req)
	}
	q.requests = make(map[string]*PendingRequest)
	return result
}
