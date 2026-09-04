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
	curStep := 0
	cat := ""
	if m.errScheduler != nil {
		curStep = m.errScheduler.OnRequest(sessionID, m.config.model, req.Request.Prompt)
		cat = m.errScheduler.SessionCategory(sessionID)
	}

	queueSize := 0
	freeSlots := 0
	if m.pendingQueue != nil {
		queueSize = m.pendingQueue.Size()
	}
	if m.capacityProvider != nil {
		freeSlots = m.capacityProvider.FreeSlots()
	}
	log.Debug().Msgf("[ERR] recv: session=%s curStep=%d cat=%s curQueue=%d free=%d",
		sessionID, curStep, cat, queueSize, freeSlots)

	m.dispatchMu.Lock()
	defer m.dispatchMu.Unlock()

	if m.pendingQueue != nil {
		m.dispatchWithQueue(msg, curStep)
	} else {
		priority := m.computePrioritySingle(sessionID, curStep, req.Request.Prompt)
		m.dispatchDirect(msg, priority)
	}
}

func (m *GlobalSchedulerManager) computePrioritySingle(sessionID string, curStep int, prompt string) int {
	if sessionID == "" {
		return int(m.errScheduler.AnonymousPriority(prompt))
	}
	return int(m.errScheduler.Priority(sessionID, curStep, prompt))
}

// dispatchWithQueue enqueues a request. On full queue, rejects the request
// with lower curStep (less progress, better UX) or evicts the lowest-curStep
// queued request if the new one has more progress.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchWithQueue(msg *ControlMessage, curStep int) {
	req := msg.Request.(*ScheduleRequestMsg)

	if m.pendingQueue.IsFull() {
		victim := m.pendingQueue.PeekLowestStep()
		if victim != nil && curStep <= victim.CurStep {
			log.Warn().Msgf("[ERR] queue full, rejecting session %s (curStep=%d) — victim has curStep=%d (more progress, kept)",
				req.SessionID, curStep, victim.CurStep)
			msg.Response <- ErrQueueFull
			return
		}
		victim = m.pendingQueue.EvictLowestStep()
		if victim != nil {
			log.Warn().Msgf("[ERR] queue full, evicting session %s (curStep=%d, waited=%v) for session %s (curStep=%d)",
				victim.SessionID, victim.CurStep, time.Since(victim.EnqueueTime),
				req.SessionID, curStep)
			victim.Msg.Response <- ErrQueueFull
		}
	}

	if err := m.pendingQueue.Add(&PendingRequest{
		ReqID:     req.Request.ReqId,
		SessionID: req.SessionID,
		CurStep:   curStep,
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

		pr := m.pendingQueue.Peek(func(r *PendingRequest) int {
			req := r.Msg.Request.(*ScheduleRequestMsg)
			if r.SessionID == "" {
				return int(m.errScheduler.AnonymousPriorityWith(snap, req.Request.Prompt))
			}
			return int(m.errScheduler.PriorityWith(snap, r.SessionID, r.CurStep, req.Request.Prompt))
		})
		if pr == nil {
			return
		}

		req, ok := pr.Msg.Request.(*ScheduleRequestMsg)
		if !ok {
			pr.Msg.Response <- fmt.Errorf("unknown schedule message type in queue")
			m.pendingQueue.Remove(pr.ReqID)
			continue
		}

		if req.ReqCTX != nil {
			select {
			case <-req.ReqCTX.Done():
				pr.Msg.Response <- ErrNoInstance
				m.pendingQueue.Remove(pr.ReqID)
				continue
			default:
			}
		}

		result := m.scheduler.schedule(req, nil)

		if result.PrefillUrl == "" {
			return
		}

		m.pendingQueue.Remove(pr.ReqID)
		priority := m.computePriorityWithSnapshot(snap, pr.SessionID, pr.CurStep, req.Request.Prompt)
		m.dispatchScheduled(pr.Msg, req, result, priority)
	}
}

func (m *GlobalSchedulerManager) computePriorityWithSnapshot(snap *algoSnapshot, sessionID string, curStep int, prompt string) int {
	if sessionID == "" {
		return int(m.errScheduler.AnonymousPriorityWith(snap, prompt))
	}
	return int(m.errScheduler.PriorityWith(snap, sessionID, curStep, prompt))
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

	if req.Request.ReqId != "" {
		m.errInFlight[req.Request.ReqId] = errInFlightEntry{
			SessionID:    req.SessionID,
			PrefillURL:   result.PrefillUrl,
			DecodeURL:    result.DecodeUrl,
			DispatchTime: time.Now(),
		}
	}

	inFlight := 0
	totalSlots := 0
	queueSize := 0
	if m.capacityProvider != nil {
		inFlight = int(m.capacityProvider.InFlight())
		totalSlots = m.capacityProvider.TotalSlots()
	}
	if m.pendingQueue != nil {
		queueSize = m.pendingQueue.Size()
	}
	log.Debug().Msgf("[ERR] dispatch: session=%s curStep=%d cat=%s priority=%d -> %s inFlight=%d/%d queue=%d",
		req.SessionID, m.errScheduler.SessionIteration(req.SessionID),
		m.errScheduler.SessionCategory(req.SessionID), priority,
		result.PrefillUrl, inFlight, totalSlots, queueSize)

	req.Request.Prompt = ""
	req.Request.PromptToken = nil

	msg.Response <- &SuggestionResultMsg{
		PrefillUrl: result.PrefillUrl,
		DecodeUrl:  result.DecodeUrl,
		DpRank:     result.DpRank,
		Priority:   &priority,
	}
}

// errInFlightEntry records the dispatch metadata for a single in-flight ERR
// request, keyed by reqID in m.errInFlight. Used to make cleanup idempotent:
// a reqID can only be released once, so OnCompleted/RemoveRequest/session
// side-effects cannot double-fire even if onInstanceUnhealthy and ErrOnResponse
// race for the same request.
type errInFlightEntry struct {
	SessionID    string
	PrefillURL   string
	DecodeURL    string
	DispatchTime time.Time
}

// ErrInFlightCount returns the number of dispatched-but-unreleased ERR
// requests. Returns 0 when ERR scheduling is disabled.
func (m *GlobalSchedulerManager) ErrInFlightCount() int {
	if m.errScheduler == nil {
		return 0
	}
	m.dispatchMu.Lock()
	defer m.dispatchMu.Unlock()
	return len(m.errInFlight)
}

// checkErrInFlightSurvival force-releases in-flight entries whose completion
// callback was lost (e.g. a handler panic after dispatch), mirroring what
// checkReqSurvival does for the LB's reqSet. Threshold is SessionTimeoutSec
// (well above any legitimate single-round latency); 0 falls back to
// defaultErrReqSurvivalSec. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) checkErrInFlightSurvival() {
	threshold := time.Duration(m.errScheduler.config.SessionTimeoutSec) * time.Second
	if threshold <= 0 {
		threshold = defaultErrReqSurvivalSec * time.Second
	}
	now := time.Now()
	for reqID, entry := range m.errInFlight {
		if now.Sub(entry.DispatchTime) < threshold {
			continue
		}
		if _, released := m.errReleaseReq(reqID); !released {
			continue
		}
		log.Warn().Msgf("[ERR] req %s (session %s) zombie-released after %v, completion callback lost",
			reqID, entry.SessionID, threshold)
		if entry.SessionID != "" {
			m.errScheduler.OnCancel(entry.SessionID)
			if m.pendingQueue != nil {
				m.pendingQueue.RemoveBySession(entry.SessionID)
			}
		}
	}
	m.tryDispatchFromQueue()
}

// errReleaseReq releases the in-flight tracking for reqID. Returns the entry
// and true if this call was the one that released it; returns false (no-op)
// if the request was already released or never registered.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) errReleaseReq(reqID string) (errInFlightEntry, bool) {
	if reqID == "" {
		return errInFlightEntry{}, false
	}
	entry, ok := m.errInFlight[reqID]
	if !ok {
		return errInFlightEntry{}, false
	}
	delete(m.errInFlight, reqID)
	if m.capacityProvider != nil {
		m.capacityProvider.OnCompleted()
	}
	if m.metricProvider != nil && entry.PrefillURL != "" {
		if err := m.metricProvider.RemoveRequest(&LlmRequest{ReqId: reqID}, &InstanceContext{
			InstanceID:       entry.PrefillURL,
			DecodeInstanceID: entry.DecodeURL,
		}); err != nil {
			log.Debug().Msgf("[ERR] RemoveRequest for req %s: %v", reqID, err)
		}
	}
	return entry, true
}

// ErrOnResponse is called when an HTTP response completes.
// insURL is the instance the request was forwarded to; empty if never dispatched.
// Caller does NOT hold dispatchMu — counter adjustments and drain acquire it internally.
func (m *GlobalSchedulerManager) ErrOnResponse(sessionID, reqID, prefillURL, decodeURL, finishReason string) {
	if m.errScheduler == nil {
		return
	}

	m.dispatchMu.Lock()
	_, released := m.errReleaseReq(reqID)
	if prefillURL == "" || released {
		switch finishReason {
		case "stop":
			m.errScheduler.OnFinish(sessionID)
		case "cancelled":
			m.errScheduler.OnCancel(sessionID)
			log.Warn().Msgf("[ERR] session %s stream cancelled (insURL=%s)", sessionID, prefillURL)
		default:
			m.errScheduler.RestartIdleTimer(sessionID)
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
}

// ErrOnCancel is called when an HTTP connection is closed abnormally.
// Caller does NOT hold dispatchMu — counter adjustments and drain acquire it internally.
func (m *GlobalSchedulerManager) ErrOnCancel(sessionID, reqID, prefillURL, decodeURL string) {
	if m.errScheduler == nil {
		return
	}

	m.dispatchMu.Lock()
	_, released := m.errReleaseReq(reqID)
	if prefillURL == "" || released {
		log.Warn().Msgf("[ERR] session %s connection closed (insURL=%s)", sessionID, prefillURL)
		m.errScheduler.OnCancel(sessionID)
		if m.pendingQueue != nil {
			m.pendingQueue.RemoveBySession(sessionID)
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
}

// onInstanceUnhealthy is called by InstanceManager when an instance's SSE
// connection drops. It cleans up all in-flight tracking for requests that
// were dispatched to the now-unhealthy instance, then drains the queue.
// Caller does NOT hold dispatchMu — cleanup and drain acquire it internally.
func (m *GlobalSchedulerManager) onInstanceUnhealthy(insUrl string) {
	if m.errScheduler == nil {
		return
	}

	m.dispatchMu.Lock()
	var cleaned int
	for reqID, entry := range m.errInFlight {
		if entry.PrefillURL != insUrl && entry.DecodeURL != insUrl {
			continue
		}
		if _, released := m.errReleaseReq(reqID); !released {
			continue
		}
		cleaned++
		if entry.SessionID != "" {
			m.errScheduler.OnCancel(entry.SessionID)
			if m.pendingQueue != nil {
				m.pendingQueue.RemoveBySession(entry.SessionID)
			}
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	if cleaned > 0 {
		log.Warn().Msgf("[ERR] instance %s went unhealthy, cleaned %d in-flight request(s)", insUrl, cleaned)
	}
}
