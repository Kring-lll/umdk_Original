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
	Priority    int
	Msg         *ControlMessage
	EnqueueTime time.Time
}

// PendingQueue is a per-model pending queue using a hash map.
// Pick scans all entries O(n) to find the highest priority (lowest value).
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

// Add adds a request to the pending queue.
// Returns ErrQueueFull if the queue is at maxSize and the ReqID is new.
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

// Pick removes and returns the highest-priority (lowest value) request.
// Ties broken by EnqueueTime (oldest first). Returns nil if empty.
func (q *PendingQueue) Pick() *PendingRequest {
	q.mu.Lock()
	defer q.mu.Unlock()

	if len(q.requests) == 0 {
		return nil
	}

	var best *PendingRequest
	bestPri := math.MaxInt32

	for _, req := range q.requests {
		if best == nil || req.Priority < bestPri ||
			(req.Priority == bestPri && req.EnqueueTime.Before(best.EnqueueTime)) {
			bestPri = req.Priority
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
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests)
}

// RemoveBySession removes a request by SessionID (best-effort, for cancel).
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

