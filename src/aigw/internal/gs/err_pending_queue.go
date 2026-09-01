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
	K           int
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
	if _, exists := q.requests[req.ReqID]; !exists && q.maxSize > 0 && len(q.requests) >= q.maxSize {
		return ErrQueueFull
	}
	q.requests[req.ReqID] = req
	return nil
}

// Pick removes and returns the highest-priority request. Priority is computed
// on-the-fly by fn. Ties broken by EnqueueTime (oldest first).
func (q *PendingQueue) Pick(fn func(*PendingRequest) int) *PendingRequest {
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

	if best != nil {
		delete(q.requests, best.ReqID)
	}
	return best
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

func (q *PendingQueue) RemoveBySession(sessionID string) {
	if sessionID == "" {
		return
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	for id, req := range q.requests {
		if req.SessionID == sessionID {
			delete(q.requests, id)
			return
		}
	}
}

// EvictLowestK finds and removes the request with the smallest K (least progress).
// Among same-K requests, picks the one with the shortest wait (most recent EnqueueTime).
// Returns the evicted request, or nil if the queue is empty.
func (q *PendingQueue) EvictLowestK() *PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	if len(q.requests) == 0 {
		return nil
	}

	// Scan from most-recently-enqueued to find the smallest K.
	// If we hit K=1, stop early (can't go lower).
	var victim *PendingRequest
	for _, req := range q.requests {
		if victim == nil {
			victim = req
			continue
		}
		if req.K < victim.K {
			victim = req
			if req.K <= 1 {
				break
			}
		} else if req.K == victim.K && req.EnqueueTime.After(victim.EnqueueTime) {
			// Same K: prefer shorter wait (more recent enqueue)
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
