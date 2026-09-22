/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: ERR dispatch logic — queue management and request scheduling.
 * Create: 2026-08-25
 */

package gs

import (
	"fmt"
	"math"
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

	// Advisory callers forward themselves: ERR must not queue, track, or take slots.
	if req.Advisory {
		m.dispatchMu.Lock()
		m.dispatchAdvisory(msg)
		m.dispatchMu.Unlock()
		return
	}

	sessionID := req.SessionID
	curStep := 0
	cat := ""
	if m.errScheduler != nil {
		curStep = m.errScheduler.OnRequest(sessionID, m.config.model, classifyPrompt(req))
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
		priority := m.computePrioritySingle(sessionID, curStep, classifyPrompt(req))
		m.dispatchDirect(msg, priority)
	}
}

// classifyPrompt returns the classification input: user-role content when
// supplied (system prompts would dominate the TF-IDF vector), else the full prompt.
func classifyPrompt(req *ScheduleRequestMsg) string {
	if req != nil && req.Request != nil {
		if req.Request.UserPrompt != "" {
			return req.Request.UserPrompt
		}
		return req.Request.Prompt
	}
	return ""
}

func (m *GlobalSchedulerManager) computePrioritySingle(sessionID string, curStep int, prompt string) int {
	if sessionID == "" {
		return math.MinInt32
	}
	return int(m.errScheduler.Priority(sessionID, curStep, prompt))
}

// dispatchWithQueue enqueues a request. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchWithQueue(msg *ControlMessage, curStep int) {
	req := msg.Request.(*ScheduleRequestMsg)

	if m.pendingQueue.IsFull() {
		victim := m.pendingQueue.PeekLowestStep()
		if victim != nil && curStep <= victim.CurStep {
			log.Warn().Msgf("[ERR] queue full, rejecting session %s (curStep=%d) — victim has curStep=%d (more progress, kept)",
				req.SessionID, curStep, victim.CurStep)
			m.rollbackFailedAttempt(req.SessionID, 0)
			msg.Response <- ErrQueueFull
			return
		}
		victim = m.pendingQueue.EvictLowestStep()
		if victim != nil {
			log.Warn().Msgf("[ERR] queue full, evicting session %s (curStep=%d, waited=%v) for session %s (curStep=%d)",
				victim.SessionID, victim.CurStep, time.Since(victim.EnqueueTime),
				req.SessionID, curStep)
			m.rollbackFailedAttempt(victim.SessionID, time.Since(victim.StarveClock))
			victim.Msg.Response <- ErrQueueFull
		}
	}

	if err := m.pendingQueue.Add(&PendingRequest{
		ReqID:       req.Request.ReqId,
		SessionID:   req.SessionID,
		CurStep:     curStep,
		Msg:         msg,
		StarveClock: time.Now().Add(-m.errScheduler.TakeQueueWaitCarry(req.SessionID)),
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
		// With engine-signal admission active, the aigw-side global capacity
		// gate is superseded (the waiting-based gate below is the authority).
		if m.kvAdmission == nil && m.capacityProvider != nil && !m.capacityProvider.HasCapacity() {
			return
		}

		boostDur := time.Duration(m.errScheduler.config.QueueBoostSec) * time.Second
		pr := m.pendingQueue.Peek(func(r *PendingRequest) int {
			req := r.Msg.Request.(*ScheduleRequestMsg)
			var p int
			if r.SessionID == "" {
				// Anonymous internal calls (context compression / checkpoints)
				// are small, latency-sensitive, and synchronously block the
				// parent session — whose idle timer keeps running while the
				// anonymous request queues. Rank them at the boost tier.
				p = math.MinInt32
			} else {
				p = int(m.errScheduler.PriorityWith(snap, r.SessionID, r.CurStep, classifyPrompt(req)))
			}
			if boostDur > 0 && time.Since(r.StarveClock) >= boostDur {
				p = math.MinInt32
			}
			return p
		})
		if pr == nil {
			return
		}
		if boostDur > 0 && time.Since(pr.StarveClock) >= boostDur {
			log.Warn().Msgf("[ERR] req %s (session %s) queue-boosted to top priority after waiting %v (threshold %v)",
				pr.ReqID, pr.SessionID, time.Since(pr.StarveClock).Round(time.Second), boostDur)
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
				if pr.SessionID != "" {
					// Rollback here, exactly once: the request left the queue.
					m.errScheduler.OnQueueTimeout(pr.SessionID, time.Since(pr.StarveClock))
				}
				continue
			default:
			}
		}

		result := m.scheduler.schedule(req, nil)

		if result.PrefillUrl == "" {
			return
		}

		// Admission gate: hold the request in queue when the target engine
		// reports waiting>0 (or signals are stale and the fallback
		// in-flight gate says no). Stays queued — never rejected.
		if m.kvAdmission != nil && !m.kvAdmission.AllowDispatch(result.PrefillUrl) {
			return
		}

		m.pendingQueue.Remove(pr.ReqID)
		priority := m.computePriorityWithSnapshot(snap, pr.SessionID, pr.CurStep, classifyPrompt(req))
		// StarveClock age at dispatch = inherited carry + this attempt's
		// queue wait — re-credited in full on an in-flight abort.
		m.dispatchScheduled(pr.Msg, req, result, priority, false, time.Since(pr.StarveClock))
	}
}

func (m *GlobalSchedulerManager) computePriorityWithSnapshot(snap *algoSnapshot, sessionID string, curStep int, prompt string) int {
	if sessionID == "" {
		return math.MinInt32
	}
	return int(m.errScheduler.PriorityWith(snap, sessionID, curStep, prompt))
}

// dispatchAdvisory: query-only requests — LB + hint, no ERR books;
// AddRequest stays on the k8s lifecycle. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchAdvisory(msg *ControlMessage) {
	req := msg.Request.(*ScheduleRequestMsg)
	priority := int(m.errScheduler.AnonymousPriority(classifyPrompt(req)))
	m.dispatchScheduled(msg, req, m.scheduler.schedule(req, nil), priority, true, 0)
}

// dispatchDirect dispatches immediately without queueing (passthrough mode).
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchDirect(msg *ControlMessage, priority int) {
	req := msg.Request.(*ScheduleRequestMsg)

	result := m.scheduler.schedule(req, nil)

	if result.PrefillUrl == "" {
		m.rollbackFailedAttempt(req.SessionID, 0)
		msg.Response <- &SuggestionResultMsg{}
		return
	}

	m.dispatchScheduled(msg, req, result, priority, false, 0)
}

// rollbackFailedAttempt undoes the k++ of a pre-dispatch failure and
// carries the starved wait into the session. Exactly once per attempt.
func (m *GlobalSchedulerManager) rollbackFailedAttempt(sessionID string, waited time.Duration) {
	if m.errScheduler != nil {
		m.errScheduler.OnQueueTimeout(sessionID, waited)
	}
}

// dispatchScheduled is the shared dispatch step for all modes: LB view,
// KVC hooks, ERR in-flight books, response. Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) dispatchScheduled(msg *ControlMessage, req *ScheduleRequestMsg,
	result *ScheduleResult, priority int, advisory bool, queueWait time.Duration) {

	m.recordScheduleStats(result)

	if result.PrefillUrl != "" {
		if err := m.metricProvider.AddRequest(req.Request, &InstanceContext{
			InstanceID:       result.PrefillUrl,
			GroupID:          result.PrefillGroupID,
			DecodeInstanceID: result.DecodeUrl,
		}); err != nil {
			if !advisory {
				m.rollbackFailedAttempt(req.SessionID, 0)
			}
			msg.Response <- fmt.Errorf("failed to add request to provider: %v", err)
			return
		}
	}

	if m.kvcSessionMgr != nil {
		sessionID := headerVal(req.Headers, "x-session-id")
		agentID := headerVal(req.Headers, "x-agent-id")
		m.kvcSessionMgr.OnRequestScheduled(sessionID, agentID, result.PrefillUrl, nil)
	}

	if !advisory {
		m.capacityProvider.OnDispatched()

		if req.Request.ReqId != "" {
			m.errInFlight[req.Request.ReqId] = errInFlightEntry{
				SessionID:    req.SessionID,
				PrefillURL:   result.PrefillUrl,
				DecodeURL:    result.DecodeUrl,
				DispatchTime: time.Now(),
				QueueWait:    queueWait,
			}
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
	req.Request.UserPrompt = ""
	req.Request.PromptToken = nil

	if !msg.Deliver(&SuggestionResultMsg{
		PrefillUrl: result.PrefillUrl,
		DecodeUrl:  result.DecodeUrl,
		DpRank:     result.DpRank,
		Priority:   &priority,
	}) {
		m.undoDispatched(req, result, advisory)
	}
}

// undoDispatched rolls back a dispatch the requester abandoned.
// Caller must hold m.dispatchMu.
func (m *GlobalSchedulerManager) undoDispatched(req *ScheduleRequestMsg,
	result *ScheduleResult, advisory bool) {
	log.Warn().Msgf("[ERR] req %s (session %s) dispatch abandoned by requester — releasing accounting",
		req.Request.ReqId, req.SessionID)
	if advisory {
		// Advisory takes no ERR slot; only the LB view needs undoing.
		if m.metricProvider != nil && result.PrefillUrl != "" {
			if err := m.metricProvider.RemoveRequest(req.Request, &InstanceContext{
				InstanceID:       result.PrefillUrl,
				GroupID:          result.PrefillGroupID,
				DecodeInstanceID: result.DecodeUrl,
			}); err != nil {
				log.Debug().Msgf("[ERR] RemoveRequest for abandoned req %s: %v", req.Request.ReqId, err)
			}
		}
		return
	}
	entry, released := m.errReleaseReq(req.Request.ReqId)
	if released && entry.SessionID != "" {
		m.errScheduler.RollbackInFlightAttempt(entry.SessionID, entry.QueueWait)
	}
}

// errInFlightEntry: keyed by reqID; release is idempotent (first releaser wins).
type errInFlightEntry struct {
	SessionID    string
	PrefillURL   string
	DecodeURL    string
	DispatchTime time.Time
	// QueueWait is this attempt's total starvation-clock age at dispatch
	// (inherited carry included). On an in-flight abort the retry re-enters
	// the queue with this much credit, so a request cancelled shortly
	// after dispatch does not restart its boost clock from zero.
	QueueWait time.Duration
}

// ErrInFlightCount returns dispatched-but-unreleased ERR requests (0 if ERR is off).
func (m *GlobalSchedulerManager) ErrInFlightCount() int {
	if m.errScheduler == nil {
		return 0
	}
	m.dispatchMu.Lock()
	defer m.dispatchMu.Unlock()
	return len(m.errInFlight)
}

// checkErrInFlightSurvival force-releases entries whose completion callback
// was lost; threshold is SessionTimeoutSec. Caller must hold m.dispatchMu.
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
		// Release ONLY the leaked entry — the session may still be alive.
		log.Warn().Msgf("[ERR] req %s (session %s) zombie-released after %v, completion callback lost",
			reqID, entry.SessionID, threshold)
	}
	m.tryDispatchFromQueue()
}

// errReleaseReq idempotently releases reqID; reports whether this call was
// the releaser. Caller must hold m.dispatchMu.
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
	entry, released := m.errReleaseReq(reqID)
	if prefillURL == "" || released {
		switch finishReason {
		case "stop":
			m.errScheduler.OnFinish(sessionID)
		case "cancelled":
			m.errScheduler.RollbackInFlightAttempt(sessionID, entry.QueueWait)
			log.Warn().Msgf("[ERR] session %s stream cancelled (insURL=%s), k rolled back awaiting retry", sessionID, prefillURL)
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
	entry, released := m.errReleaseReq(reqID)
	if prefillURL == "" || released {
		log.Warn().Msgf("[ERR] session %s connection closed (insURL=%s)", sessionID, prefillURL)

		var removed []*PendingRequest
		if m.pendingQueue != nil {
			removed = m.pendingQueue.RemoveBySession(sessionID)
			for _, pr := range removed {
				pr.Msg.Response <- ErrNoInstance
			}
		}

		if prefillURL == "" {
			// Never dispatched: roll back k, carry the starved wait.
			queued := false
			starved := time.Duration(0)
			for _, pr := range removed {
				if pr.ReqID == reqID {
					queued = true
				}
				if w := time.Since(pr.StarveClock); w > starved {
					starved = w
				}
			}
			if queued {
				m.errScheduler.OnQueueTimeout(sessionID, starved)
			} else {
				// Rejected before queueing: keep the progress, arm the idle timer.
				m.errScheduler.RestartIdleTimer(sessionID)
			}
		} else {
			m.errScheduler.RollbackInFlightAttempt(sessionID, entry.QueueWait)
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()
}

// onInstanceUnhealthy releases in-flight tracking for requests dispatched
// to the now-unhealthy instance, then drains the queue.
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
			m.errScheduler.RollbackInFlightAttempt(entry.SessionID, entry.QueueWait)
		}
	}
	m.tryDispatchFromQueue()
	m.dispatchMu.Unlock()

	if cleaned > 0 {
		log.Warn().Msgf("[ERR] instance %s went unhealthy, released %d in-flight request(s), sessions kept for retry", insUrl, cleaned)
	}
}
