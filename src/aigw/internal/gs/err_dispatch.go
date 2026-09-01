/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR dispatch logic — queue management and request scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"fmt"
	"time"

	"huawei.com/aigw/pkg/log"
)

// dispatchErr is the ERR-aware entry point for handling schedule requests.
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

	// Fast-fail: no instances registered → 503 (queue mode only)
	if m.pendingQueue != nil && m.capacityProvider != nil && m.capacityProvider.InstanceCount() == 0 {
		msg.Response <- ErrNoInstance
		return
	}

	sessionID := req.SessionID
	k := 0
	cat := ""
	if m.errScheduler != nil {
		k = m.errScheduler.OnRequest(sessionID, m.config.model, req.Request.Prompt)
		cat = m.errScheduler.SessionCategory(sessionID)
	}

	log.Debug().Msgf("[ERR] recv: session=%s k=%d cat=%s queue=%d free=%d",
		sessionID, k, cat, m.pendingQueue.Size(), m.capacityProvider.FreeSlots())

	m.dispatchMu.Lock()
	defer m.dispatchMu.Unlock()

	if m.pendingQueue != nil {
		m.dispatchWithQueue(msg, k)
	} else {
		priority := m.computePrioritySingle(sessionID, k, req.Request.Prompt)
		m.dispatchDirect(msg, priority)
	}
}

func (m *GlobalSchedulerManager) computePrioritySingle(sessionID string, k int, prompt string) int {
	if sessionID == "" {
		return int(m.errScheduler.AnonymousPriority(prompt))
	}
	return int(m.errScheduler.Priority(sessionID, k, prompt))
}

// dispatchWithQueue enqueues the request and attempts to dispatch from queue.
// When the queue is full, evicts the lowest-K request (503 it) and enqueues the new one.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchWithQueue(msg *ControlMessage, k int) {
	req := msg.Request.(*ScheduleRequestMsg)

	if m.pendingQueue.IsFull() {
		victim := m.pendingQueue.EvictLowestK()
		if victim != nil {
			log.Warn().Msgf("[ERR] queue full, evicting session %s (k=%d, waited=%v) for session %s (k=%d)",
				victim.SessionID, victim.K, time.Since(victim.EnqueueTime),
				req.SessionID, k)
			victim.Msg.Response <- ErrQueueFull
		}
	}

	if err := m.pendingQueue.Add(&PendingRequest{
		ReqID:     req.Request.ReqId,
		SessionID: req.SessionID,
		K:         k,
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

	snap := m.errScheduler.AlgoSnapshot()

	for {
		if m.pendingQueue.IsEmpty() {
			return
		}
		if m.capacityProvider != nil && !m.capacityProvider.HasCapacity() {
			return
		}

		pr := m.pendingQueue.Pick(func(r *PendingRequest) int {
			req := r.Msg.Request.(*ScheduleRequestMsg)
			if r.SessionID == "" {
				return int(m.errScheduler.AnonymousPriorityWith(snap, req.Request.Prompt))
			}
			return int(m.errScheduler.PriorityWith(snap, r.SessionID, r.K, req.Request.Prompt))
		})
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
				pr.Msg.Response <- ErrNoInstance
				continue
			default:
			}
		}

		result := m.scheduler.schedule(req, nil)

		if result.PrefillUrl == "" {
			pr.Msg.Response <- ErrNoInstance
			continue
		}

		priority := m.computePriorityWithSnapshot(snap, pr.SessionID, pr.K, req.Request.Prompt)
		m.dispatchScheduled(pr.Msg, req, result, priority)
	}
}

func (m *GlobalSchedulerManager) computePriorityWithSnapshot(snap *algoSnapshot, sessionID string, k int, prompt string) int {
	if sessionID == "" {
		return int(m.errScheduler.AnonymousPriorityWith(snap, prompt))
	}
	return int(m.errScheduler.PriorityWith(snap, sessionID, k, prompt))
}

// dispatchDirect dispatches immediately without queueing (passthrough mode).
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchDirect(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	result := m.scheduler.schedule(req, nil)

	if result.PrefillUrl == "" {
		msg.Response <- &SuggestionResultMsg{}
		return
	}

	m.dispatchScheduled(msg, req, result, priority)
}

// dispatchScheduled is the shared dispatch step for both passthrough and queue modes.
// Feeds RR's instance capacity (AddRequest), KVC hooks, model-level in-flight tracking,
// writes the response with priority hint. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchScheduled(msg *ControlMessage, req *ScheduleRequestMsg,
	result *ScheduleResult, priority int) {

	m.recordScheduleStats(result)

	if result.PrefillUrl != "" {
		if err := m.metricProvider.AddRequest(req.Request, &InstanceContext{
			InstanceID:       result.PrefillUrl,
			GroupID:          result.PrefillGroupID,
			DecodeInstanceID: result.DecodeUrl,
		}); err != nil {
			msg.Response <- fmt.Errorf("failed to add request to provider: %v", err)
			return
		}
	}

	if m.kvcSessionMgr != nil {
		sessionID := headerVal(req.Headers, "x-session-id")
		agentID := headerVal(req.Headers, "x-agent-id")
		m.kvcSessionMgr.OnRequestScheduled(sessionID, agentID, result.PrefillUrl, nil)
	}

	m.capacityProvider.OnDispatched()

	log.Debug().Msgf("[ERR] dispatch: session=%s k=%d cat=%s priority=%d -> %s inFlight=%d/%d queue=%d",
		req.SessionID, m.errScheduler.SessionIteration(req.SessionID),
		m.errScheduler.SessionCategory(req.SessionID), priority,
		result.PrefillUrl, m.capacityProvider.InFlight(), m.capacityProvider.TotalSlots(), m.pendingQueue.Size())

	req.Request.Prompt = ""
	req.Request.PromptToken = nil

	msg.Response <- &SuggestionResultMsg{
		PrefillUrl: result.PrefillUrl,
		DecodeUrl:  result.DecodeUrl,
		DpRank:     result.DpRank,
		Priority:   &priority,
	}
}

// ErrOnResponse is called when an HTTP response completes.
// insURL is the instance the request was forwarded to; empty if never dispatched.
// Caller does NOT hold dispatchMu — counter adjustments and drain acquire it internally.
func (m *GlobalSchedulerManager) ErrOnResponse(sessionID, reqID, prefillURL, decodeURL, finishReason string) {
	if m.errScheduler == nil {
		return
	}

	switch finishReason {
	case "stop":
		m.errScheduler.OnFinish(sessionID)
	case "cancelled":
		m.errScheduler.OnCancel(sessionID)
		log.Warn().Msgf("[ERR] session %s stream cancelled (insURL=%s)", sessionID, prefillURL)
	default:
		m.errScheduler.RestartIdleTimer(sessionID)
	}

	m.dispatchMu.Lock()
	if prefillURL != "" {
		if m.capacityProvider != nil {
			m.capacityProvider.OnCompleted()
		}
		if reqID != "" {
			if err := m.metricProvider.RemoveRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{
				InstanceID:       prefillURL,
				DecodeInstanceID: decodeURL,
			}); err != nil {
				log.Debug().Msgf("[ERR] RemoveRequest failed for req %s: %v", reqID, err)
			}
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
}

// ErrOnCancel is called when an HTTP connection is closed abnormally.
func (m *GlobalSchedulerManager) ErrOnCancel(sessionID, reqID, prefillURL, decodeURL string) {
	if m.errScheduler == nil {
		return
	}

	log.Warn().Msgf("[ERR] session %s connection closed (insURL=%s)", sessionID, prefillURL)
	m.errScheduler.OnCancel(sessionID)

	m.dispatchMu.Lock()
	if prefillURL != "" {
		if m.capacityProvider != nil {
			m.capacityProvider.OnCompleted()
		}
		if reqID != "" {
			if err := m.metricProvider.RemoveRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{
				InstanceID:       prefillURL,
				DecodeInstanceID: decodeURL,
			}); err != nil {
				log.Debug().Msgf("[ERR] RemoveRequest failed for req %s: %v", reqID, err)
			}
		}
	}
	if m.pendingQueue != nil {
		m.pendingQueue.RemoveBySession(sessionID)
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
}
