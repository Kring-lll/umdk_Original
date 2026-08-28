/*
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
// It replaces the original handleSchedule logic when ERR is enabled.
//
// Flow:
//  1. Compute ERR priority (iteration k → E[rem|k])
//  2. If queue mode (N>0): enqueue, then tryDispatchFromQueue
//  3. If passthrough mode (N=0): dispatch directly with priority
func (m *GlobalSchedulerManager) dispatchErr(msg *ControlMessage) {
	req, ok := msg.Request.(*ScheduleRequestMsg)
	if !ok {
		msg.Response <- fmt.Errorf("unknown schedule message type")
		return
	}

	// Check context cancellation
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

	// ERR: compute iteration and priority. The algorithm returns a float E[rem]
	// (or MEJ); we truncate to int here so the value used for local queue
	// ordering and written to the downstream hint is the SAME int. See [31]/[6].
	// (Revert to float if the downstream protocol switches to float.)
	if m.errScheduler != nil {
		if sessionID == "" {
			// Anonymous request (no session_id): cannot track k across rounds.
			// Place at p50 of the step distribution instead of k=1 (which would
			// always dispatch last). See review E1.
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
		// Queue mode: enqueue then try to dispatch
		m.dispatchWithQueue(msg, priority)
	} else {
		// Passthrough mode: dispatch directly
		m.dispatchDirect(msg, priority)
	}
}

// dispatchDirect dispatches immediately without queueing. Used in passthrough
// mode (queueSize=0) and as the A1 force-forward path when the queue is full.
// It does NOT gate on capacity (the downstream LLM queues on its own); only
// capacityProvider.inFlight is bumped for tracking. See review A1/A2.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchDirect(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	result := m.scheduler.schedule(req, nil)
	m.recordScheduleStats(result)

	if result.PrefillUrl != "" {
		// ERR capacity tracking uses capacityProvider (in-flight), NOT
		// metricProvider.ReqNum. We intentionally do NOT call AddRequest: ReqNum
		// is the k8s LB gate and would block subsequent RR selections forever
		// (RemoveRequest is not wired on the ERR completion path). See A2 / decision ①.
		if m.capacityProvider != nil {
			m.capacityProvider.OnDispatched(result.PrefillUrl)
		}
	}

	msg.Response <- &SuggestionResultMsg{
		PrefillUrl: result.PrefillUrl,
		DecodeUrl:  result.DecodeUrl,
		DpRank:     result.DpRank,
		Priority:   &priority,
	}
}

// dispatchWithQueue enqueues the request and then attempts to dispatch
// from the queue. The request competes fairly with all queued requests.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchWithQueue(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	// A1: when the queue is already full, forward the highest-priority among
	// (new request, queue front) and enqueue the other. This bounds the aigw
	// queue at maxSize AND preserves priority ordering: a low-priority new
	// request cannot jump ahead of higher-priority queued ones (the old code
	// forwarded the new request unconditionally, violating priority). The
	// downstream LLM has its own queue, so over-subscribing an instance here is
	// acceptable. See review A1/[15].
	if m.pendingQueue.IsFull() {
		picked := m.pendingQueue.Pick() // highest priority (lowest value) in queue
		if picked == nil {
			// IsFull() but Pick nil (impossible under m.dispatchMu): fall back.
			log.Warn().Msgf("[ERR] queue full but Pick nil, force-forwarding session %s", req.SessionID)
			m.dispatchDirect(msg, priority)
			return
		}
		if priority < picked.Priority {
			// New request is higher priority than the queue's best: forward it,
			// put picked back (Pick freed a slot, Add always fits here).
			log.Debug().Msgf("[ERR] queue full, new req %s higher priority, forwarding it", req.SessionID)
			m.dispatchDirect(msg, priority)
			if err := m.pendingQueue.Add(picked); err != nil {
				picked.Msg.Response <- err
			}
		} else {
			// Queue's best is >= new: forward the picked, enqueue the new.
			log.Debug().Msgf("[ERR] queue full, forwarding queued req %s (priority %d >= new %s priority %d)",
				picked.SessionID, picked.Priority, req.SessionID, priority)
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
		// IsFull()+Add is guarded by m.dispatchMu (no TOCTOU); this only fires
		// if a concurrent path filled the queue. Refuse rather than block forever.
		msg.Response <- err
		return
	}

	m.tryDispatchFromQueue()
}

// tryDispatchFromQueue picks requests from the queue and dispatches them
// until the queue is empty or LB.schedule() returns no instance (no capacity).
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

		// Check context cancellation for queued request
		if req.ReqCTX != nil {
			select {
			case <-req.ReqCTX.Done():
				log.Debug().Msgf("[ERR] queued request %s cancelled", pr.SessionID)
				continue // just drop it, don't re-enqueue
			default:
			}
		}

		result := m.scheduler.schedule(req, nil)

		// A2: capacity gate via capacityProvider.HasFreeSlot, NOT
		// metricProvider.ReqNum. schedule() (RR) returns a candidate instance
		// (ReqNum is not bumped on the ERR path, so RR always has a candidate
		// when any instance is registered); we only dispatch if it has a free
		// slot. If no candidate or no slot, re-enqueue and wait for a
		// completion to free capacity (ErrOnResponse re-triggers this). See A2.
		if result.PrefillUrl == "" || (m.capacityProvider != nil && !m.capacityProvider.HasFreeSlot(result.PrefillUrl)) {
			// No capacity → put back in queue and stop. Re-adding a just-picked
			// item always fits (size dropped by one on Pick); the error is only
			// a defensive guard against impossible races.
			if err := m.pendingQueue.Add(pr); err != nil {
				pr.Msg.Response <- err
				continue
			}
			log.Debug().Msgf("[ERR] no capacity, request %s stays in queue (size=%d)",
				pr.SessionID, m.pendingQueue.Size())
			return
		}

	// Got an instance with a free slot → dispatch
	m.recordScheduleStats(result)

	if m.capacityProvider != nil {
		m.capacityProvider.OnDispatched(result.PrefillUrl)
	}

	// TODO(cancel-vs-dispatch leak) — EXISTING BUG (not a new feature), risk:
	// capacity slot leak. Between the req.ReqCTX.Done() check above (:189) and
	// this OnDispatched+rsp write, the handler's ctx can fire. If the handler's
	// GetSuggestion select then picks ctx.Done() over the (buffered) rsp, the
	// handler bails via ErrOnCancel(insURL="") → OnCompleted is skipped → this
	// OnDispatched +1 is never balanced → the instance slot leaks permanently
	// (phantom in-flight, no real forward to complete it). Window is tiny (a
	// few instructions) and rare, but leaks are cumulative in long runs.
	// Correct fix is on the HANDLER side: GetSuggestion's select, on ctx.Done(),
	// must non-blocking-drain rsp and, if a suggestion was written (dispatched),
	// free that instance slot. A dispatch-side fix (OnCompleted here if
	// ctx.Done()) is WRONG — it double-frees when the handler actually got rsp
	// and proceeds, causing transient over-subscription. Defer until the
	// handler→capacityProvider plumbing + tests land. See [20]-mislabeled note.

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
// It frees capacity, updates the ERR distribution, and tries to dispatch from queue.
//
// insURL is the instance THIS request was forwarded to (passed in by the HTTP
// handler from suggestion.TargetPrefillUrl, which equals result.PrefillUrl --
// the same key OnDispatched used -- so the per-instance counter keys match).
// Capacity is request/HTTP-level, NOT session-level: the URL comes from the
// request itself, never from SessionTracker. Empty insURL means the request was
// never dispatched (e.g. GetSuggestion failed) -> skip the free.
func (m *GlobalSchedulerManager) ErrOnResponse(sessionID, insURL, finishReason string) {
	if m.errScheduler == nil {
		return
	}

	// Free ERR capacity (per-instance, keyed by the request's own instance URL).
	if insURL != "" && m.capacityProvider != nil {
		m.capacityProvider.OnCompleted(insURL)
	}

	// Update ERR distribution (session-level: k/steps keyed by sessionID).
	if finishReason == "stop" {
		m.errScheduler.OnFinish(sessionID) // records dist + arms short idle timer (§5.1: stop+60s = normal completion)
	} else if finishReason == "cancelled" {
		m.errScheduler.OnCancel(sessionID) // deletes session (no timer)
	} else {
		// tool_calls / other: the iteration's reply completed but the user input
		// isn't done (the agent will send another iteration). Arm the LONG idle
		// timer so that if no next OnRequest arrives (agent hung/crashed after a
		// tool_call), the session is still TTL-cleaned — without this, a session
		// whose last activity was a tool_call reply would leak forever (OnRequest
		// canceled the timer, OnFinish only fires on stop). §5.1: tool_calls+660s
		// = extreme anomaly. See review [20].
		m.errScheduler.RestartIdleTimer(sessionID)
	}

	// Try to dispatch from queue (capacity was just freed)
	if m.pendingQueue != nil && !m.pendingQueue.IsEmpty() {
		m.dispatchMu.Lock()
		m.tryDispatchFromQueue()
		m.dispatchMu.Unlock()
	}
}

// ErrOnCancel is called when the HTTP connection is closed abnormally.
// insURL is the request's instance URL (from the handler); empty if the request
// was never dispatched (nothing to free).
func (m *GlobalSchedulerManager) ErrOnCancel(sessionID, insURL string) {
	if m.errScheduler == nil {
		return
	}

	// Free ERR capacity (per-instance, keyed by the request's own instance URL).
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
