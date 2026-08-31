/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR dispatch logic — queue management and request scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"fmt"

	"huawei.com/aigw/pkg/log"
)

// dispatchErr is the ERR-aware entry point for handling schedule requests.
// Flow: compute priority → enqueue (queue mode) or dispatch directly (passthrough).
func (m *GlobalSchedulerManager) dispatchErr(msg *ControlMessage) {
	req, ok := msg.Request.(*ScheduleRequestMsg)
	if !ok {
		msg.Response <- fmt.Errorf("unknown schedule message type")
		return
	}

	if req.ReqCTX != nil {
		select {
		case <-req.ReqCTX.Done():
			msg.Response <- fmt.Errorf("request cancelled due to timeout")
			return
		default:
		}
	}

	sessionID := req.SessionID
	var priority int

	if m.errScheduler != nil {
		if sessionID == "" {
			priority = int(m.errScheduler.AnonymousPriority(req.Request.Prompt))
			log.Debug().Msgf("[ERR] anonymous request, priority=%d (p50), algo=%s",
				priority, m.errScheduler.AlgorithmName())
		} else {
			k := m.errScheduler.OnRequest(sessionID, m.config.model, req.Request.Prompt)
			priority = int(m.errScheduler.Priority(sessionID, k, req.Request.Prompt))
			log.Debug().Msgf("[ERR] session %s, k=%d, priority=%d, algo=%s",
				sessionID, k, priority, m.errScheduler.AlgorithmName())
		}
	}

	m.dispatchMu.Lock()
	defer m.dispatchMu.Unlock()

	if m.pendingQueue != nil {
		m.dispatchWithQueue(msg, priority)
	} else {
		m.dispatchDirect(msg, priority)
	}
}

// dispatchDirect dispatches immediately without queueing.
// Does NOT gate on capacity; only bumps in-flight tracking. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchDirect(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	result := m.scheduler.schedule(req, nil)
	m.recordScheduleStats(result)

	if result.PrefillUrl != "" && m.capacityProvider != nil {
		m.capacityProvider.OnDispatched(result.PrefillUrl)
	}

	msg.Response <- &SuggestionResultMsg{
		PrefillUrl: result.PrefillUrl,
		DecodeUrl:  result.DecodeUrl,
		DpRank:     result.DpRank,
		Priority:   &priority,
	}
}

// dispatchWithQueue enqueues the request and attempts to dispatch from queue.
// When the queue is full, forwards the higher-priority of (new, queue front)
// and enqueues the other. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchWithQueue(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	if m.pendingQueue.IsFull() {
		picked := m.pendingQueue.Pick()
		if picked == nil {
			log.Warn().Msgf("[ERR] queue full but Pick nil, force-forwarding session %s", req.SessionID)
			m.dispatchDirect(msg, priority)
			return
		}
		if priority < picked.Priority {
			m.dispatchDirect(msg, priority)
			if err := m.pendingQueue.Add(picked); err != nil {
				picked.Msg.Response <- err
			}
		} else {
			m.dispatchDirect(picked.Msg, picked.Priority)
			if err := m.pendingQueue.Add(&PendingRequest{
				ReqID:     req.Request.ReqId,
				SessionID: req.SessionID,
				Priority:  priority,
				Msg:       msg,
			}); err != nil {
				msg.Response <- err
			}
		}
		return
	}

	if err := m.pendingQueue.Add(&PendingRequest{
		ReqID:     req.Request.ReqId,
		SessionID: req.SessionID,
		Priority:  priority,
		Msg:       msg,
	}); err != nil {
		msg.Response <- err
		return
	}

	m.tryDispatchFromQueue()
}

// tryDispatchFromQueue dispatches queued requests until empty or no capacity.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) tryDispatchFromQueue() {
	if m.pendingQueue == nil {
		return
	}

	for {
		if m.pendingQueue.IsEmpty() {
			return
		}

		pr := m.pendingQueue.Pick()
		if pr == nil {
			return
		}

		req, ok := pr.Msg.Request.(*ScheduleRequestMsg)
		if !ok {
			pr.Msg.Response <- fmt.Errorf("unknown schedule message type in queue")
			continue
		}

		if req.ReqCTX != nil {
			select {
			case <-req.ReqCTX.Done():
				log.Debug().Msgf("[ERR] queued request %s cancelled", pr.SessionID)
				continue
			default:
			}
		}

		result := m.scheduler.schedule(req, nil)

		if result.PrefillUrl == "" || (m.capacityProvider != nil && !m.capacityProvider.HasFreeSlot(result.PrefillUrl)) {
			if err := m.pendingQueue.Add(pr); err != nil {
				pr.Msg.Response <- err
				continue
			}
			log.Debug().Msgf("[ERR] no capacity, request %s stays in queue (size=%d)",
				pr.SessionID, m.pendingQueue.Size())
			return
		}

		m.recordScheduleStats(result)

		if m.capacityProvider != nil {
			m.capacityProvider.OnDispatched(result.PrefillUrl)
		}

		// TODO: capacity slot leak if handler ctx fires between ctx check and OnDispatched.
		log.Debug().Msgf("[ERR] dispatched from queue: session %s → %s, priority=%d",
			pr.SessionID, result.PrefillUrl, pr.Priority)

		pr.Msg.Response <- &SuggestionResultMsg{
			PrefillUrl: result.PrefillUrl,
			DecodeUrl:  result.DecodeUrl,
			DpRank:     result.DpRank,
			Priority:   &pr.Priority,
		}
	}
}

// ErrOnResponse is called when an HTTP response completes.
// Frees capacity, updates ERR distribution, and tries to dispatch from queue.
// insURL is the instance this request was forwarded to; empty if never dispatched.
func (m *GlobalSchedulerManager) ErrOnResponse(sessionID, insURL, finishReason string) {
	if m.errScheduler == nil {
		return
	}

	if insURL != "" && m.capacityProvider != nil {
		m.capacityProvider.OnCompleted(insURL)
	}

	switch finishReason {
	case "stop":
		m.errScheduler.OnFinish(sessionID)
	case "cancelled":
		m.errScheduler.OnCancel(sessionID)
	default:
		// tool_calls/other: arm long idle timer (agent may send next iteration)
		m.errScheduler.RestartIdleTimer(sessionID)
	}

	if m.pendingQueue != nil && !m.pendingQueue.IsEmpty() {
		m.dispatchMu.Lock()
		m.tryDispatchFromQueue()
		m.dispatchMu.Unlock()
	}
}

// ErrOnCancel is called when the HTTP connection is closed abnormally.
// insURL is the request's instance URL; empty if never dispatched.
func (m *GlobalSchedulerManager) ErrOnCancel(sessionID, insURL string) {
	if m.errScheduler == nil {
		return
	}

	if insURL != "" && m.capacityProvider != nil {
		m.capacityProvider.OnCompleted(insURL)
	}

	m.errScheduler.OnCancel(sessionID)

	if m.pendingQueue != nil {
		m.pendingQueue.RemoveBySession(sessionID)
	}

	if m.pendingQueue != nil && !m.pendingQueue.IsEmpty() {
		m.dispatchMu.Lock()
		m.tryDispatchFromQueue()
		m.dispatchMu.Unlock()
	}
}
