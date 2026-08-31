/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: PendingQueue — per-model priority queue for ERR scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"fmt"
	"math"
	"sync"
	"time"

	"huawei.com/aigw/pkg/log"
)

// PendingRequest represents a request held in the pending queue.
// Each request carries its own ControlMessage (with Response channel),
// so when it's dispatched, the response can be sent back to the HTTP handler.
type PendingRequest struct {
	ReqID       string
	SessionID   string // ERR session id (may be "" for anonymous); used for k-tracking, NOT as map key
	Priority    int
	Msg         *ControlMessage // contains ScheduleRequestMsg + Response channel
	EnqueueTime time.Time
}

// PendingQueue is a per-model pending queue using a hash map.
// Pick scans all entries O(n) to find the highest priority (lowest value).
// Expected queue size is 10-500, so O(n) scan is < 0.1ms.
//
// The map key is ReqID (unique per request), NOT SessionID: SessionID is not
// guaranteed present (anonymous requests) and reusing it would let anonymous
// requests overwrite each other. See review [27]/E1.
type PendingQueue struct {
	mu       sync.Mutex
	maxSize  int
	requests map[string]*PendingRequest // key = ReqID
}

// NewPendingQueue creates a new PendingQueue with the given max size.
// maxSize > 0 enables the size limit; maxSize == 0 means unbounded.
func NewPendingQueue(maxSize int) *PendingQueue {
	return &PendingQueue{
		maxSize:  maxSize,
		requests: make(map[string]*PendingRequest),
	}
}

// ErrQueueFull is returned by Add when the queue is at maxSize.
var ErrQueueFull = fmt.Errorf("pending queue full")

// Add adds a request to the pending queue.
// Returns ErrQueueFull if the queue is at maxSize and the ReqID is new.
// A request whose ReqID already exists replaces the old entry (does not count
// against the size limit). Callers that hold m.dispatchMu can do IsFull()+Add
// atomically (no TOCTOU). See review [26].
func (q *PendingQueue) Add(req *PendingRequest) error {
	if req == nil {
		return nil
	}
	q.mu.Lock()
	defer q.mu.Unlock()

	if req.ReqID == "" {
		// Defensive: a request without ReqID would collapse onto a single key.
		req.ReqID = fmt.Sprintf("auto-%p", req)
	}
	if req.EnqueueTime.IsZero() {
		req.EnqueueTime = time.Now()
	}
	if _, exists := q.requests[req.ReqID]; !exists && q.maxSize > 0 && len(q.requests) >= q.maxSize {
		return ErrQueueFull
	}
	q.requests[req.ReqID] = req
	log.Debug().Msgf("[Queue] added req %s (session %s, priority=%d), queue size=%d",
		req.ReqID, req.SessionID, req.Priority, len(q.requests))
	return nil
}

// Pick removes and returns the highest-priority (lowest priority value) request.
// Ties are broken by EnqueueTime (oldest first) for stable FIFO and to avoid
// starvation. Returns nil if the queue is empty. See review [28].
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
		log.Debug().Msgf("[Queue] picked req %s (session %s, priority=%d), remaining=%d",
			best.ReqID, best.SessionID, best.Priority, len(q.requests))
	}
	return best
}

// IsEmpty returns true if the queue has no pending requests.
func (q *PendingQueue) IsEmpty() bool {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests) == 0
}

// IsFull returns true if the queue has reached its max size.
func (q *PendingQueue) IsFull() bool {
	if q.maxSize <= 0 {
		return false
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests) >= q.maxSize
}

// Size returns the current number of pending requests.
func (q *PendingQueue) Size() int {
	q.mu.Lock()
	defer q.mu.Unlock()
	return len(q.requests)
}

// Remove removes a request by ReqID (e.g. on cancel).
func (q *PendingQueue) Remove(reqID string) {
	if reqID == "" {
		return
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	delete(q.requests, reqID)
}

// RemoveBySession removes a request by SessionID (best-effort, for cancel
// when only the session id is known). Scans the map; expected small.
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

// UpdatePriority updates the priority of a pending request (e.g. after dist update).
func (q *PendingQueue) UpdatePriority(reqID string, priority int) {
	q.mu.Lock()
	defer q.mu.Unlock()
	if req, ok := q.requests[reqID]; ok {
		req.Priority = priority
	}
}
