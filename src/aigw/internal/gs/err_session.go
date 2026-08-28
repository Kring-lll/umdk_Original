/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Session state tracking for ERR scheduler.
 * Create: 2026-08-25
 */

package gs

import (
	"sync"
	"time"

	"huawei.com/aigw/pkg/log"
)

// ErrSessionState tracks the iteration count and lifecycle of a single user input.
//
// In the integration phase (§6.5 of design doc), we assume each session_id
// corresponds to exactly one user input. Completion is detected via
// finish_reason=stop. No task_start marker is used yet (jiuwenclaw/agent-core
// don't send one in agent_hint — only session_id + parent_session_id).
type ErrSessionState struct {
	SessionID      string
	Iteration      int // current iteration number (k), starts at 0, first request sets it to 1
	Model          string
	Category       string // CatERR: prompt category (classified at OnRequest, used at OnFinish)
	LastActiveTime time.Time
	// idleTimer is armed when an iteration's reply completes (OnFinish on stop,
	// RestartIdleTimer on tool_calls/other) and canceled on OnRequest (an
	// in-flight request's HTTP state takes over liveness). On fire the session
	// is removed (review [20], no map scan). The timeout depends on the last
	// finish_reason: stop → taskIdleTimeout (normal completion, short);
	// tool_calls/other → sessionTimeout (extreme anomaly, long).
	idleTimer *time.Timer
}

// SessionTracker manages session state for all active and completed sessions.
type SessionTracker struct {
	mu                sync.RWMutex
	taskIdleTimeout   time.Duration // post-stop idle cleanup (default 60s) — normal task loop completion
	sessionTimeout    time.Duration // post-tool_calls idle cleanup (default 660s) — extreme anomaly
	sessions          map[string]*ErrSessionState
}

// NewSessionTracker creates a new SessionTracker.
//   - taskIdleTimeout: idle timeout after a stop finish_reason (§5.1: stop+60s =
//     task loop exited = normal completion). 0 disables stop-path cleanup.
//   - sessionTimeout: idle timeout after a tool_calls/other finish_reason
//     (§5.1: tool_calls+660s = agent crash/disconnect, extreme anomaly). 0
//     disables tool_calls-path cleanup.
func NewSessionTracker(taskIdleTimeout, sessionTimeout time.Duration) *SessionTracker {
	return &SessionTracker{
		taskIdleTimeout: taskIdleTimeout,
		sessionTimeout:  sessionTimeout,
		sessions:        make(map[string]*ErrSessionState),
	}
}

// armIdleTimer (re)arms the per-session idle-cleanup timer with the given
// timeout. Called when an iteration's reply completes (stop or tool_calls).
// NOT called on OnRequest: during an in-flight request, HTTP state
// (r.Context()) is the liveness/termination mechanism — arming a second one
// here would dual-track two termination methods that could race. The idle
// timer covers only the post-completion gap (where HTTP state may not fire,
// e.g. a lingering keep-alive connection after the upstream went away). In
// the formal version, the task_start marker cancels the timer instead of
// OnRequest. Caller must hold t.mu.
func (t *SessionTracker) armIdleTimer(s *ErrSessionState, timeout time.Duration) {
	if timeout <= 0 || s == nil {
		return
	}
	if s.idleTimer != nil {
		s.idleTimer.Stop()
	}
	sessionID := s.SessionID
	s.idleTimer = time.AfterFunc(timeout, func() {
		t.cleanupIdleSession(sessionID, timeout)
	})
}

// cancelIdleTimer stops the idle timer (an in-flight request takes over
// liveness tracking via HTTP state; avoids dual-tracking two termination
// methods). Caller must hold t.mu.
func (t *SessionTracker) cancelIdleTimer(s *ErrSessionState) {
	if s != nil && s.idleTimer != nil {
		s.idleTimer.Stop()
		s.idleTimer = nil
	}
}

// RestartIdleTimer arms the long (sessionTimeout) idle timer after a
// tool_calls/other finish_reason. §5.1: tool_calls+660s no new request and no
// TCP close = agent crash/disconnect (extreme anomaly) → the session is
// cleaned up (step count NOT recorded — tool_calls isn't a completion).
// Without this, a session whose last activity was a tool_call reply would
// leak forever (OnRequest canceled the timer, OnFinish only fires on stop).
// stop arms the short timer via OnFinish; cancelled deletes. Caller-facing.
// See review [20].
func (t *SessionTracker) RestartIdleTimer(sessionID string) {
	if sessionID == "" || t.sessionTimeout <= 0 {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if s, ok := t.sessions[sessionID]; ok {
		s.LastActiveTime = time.Now()
		t.armIdleTimer(s, t.sessionTimeout)
	}
}

// cleanupIdleSession is the idle-timer callback: removes the session if it has
// been inactive for >= the timeout that armed this callback. If activity
// happened since this (stale) callback was armed, it no-ops (the live timer
// will fire later). See [20].
func (t *SessionTracker) cleanupIdleSession(sessionID string, timeout time.Duration) {
	t.mu.Lock()
	defer t.mu.Unlock()
	s, ok := t.sessions[sessionID]
	if !ok {
		return // already gone (cancel or earlier cleanup)
	}
	if time.Since(s.LastActiveTime) < timeout {
		// A newer OnRequest/OnFinish/RestartIdleTimer reset LastActiveTime and
		// armed a new timer; this stale callback's Stop lost the race. No-op.
		return
	}
	if s.idleTimer != nil {
		s.idleTimer.Stop()
	}
	delete(t.sessions, sessionID)
	log.Debug().Msgf("[ERRSession] session %s idle-cleanup (no activity for %v)", sessionID, timeout)
}

// OnRequest is called when a new HTTP request arrives for a session.
// It increments the iteration counter and returns the new k value.
// If this is the first request for the session, k=1 is returned.
func (t *SessionTracker) OnRequest(sessionID, model string) int {
	if sessionID == "" {
		return 1
	}
	now := time.Now()
	t.mu.Lock()
	defer t.mu.Unlock()

	s, ok := t.sessions[sessionID]
	if !ok {
		s = &ErrSessionState{
			SessionID:      sessionID,
			Model:          model,
			LastActiveTime: now,
			Iteration:      1,
		}
		t.sessions[sessionID] = s
		// No idle timer: the in-flight request's HTTP state covers liveness.
		log.Debug().Msgf("[ERRSession] new session: %s, model: %s", sessionID, model)
		return 1
	}

	s.Iteration++
	s.LastActiveTime = now
	s.Model = model
	t.cancelIdleTimer(s) // new in-flight request takes over; stop the post-completion idle timer
	return s.Iteration
}

// OnFinish is called when finish_reason=stop is received.
// It returns the total step count and arms the short (taskIdleTimeout) idle
// timer: §5.1 stop+60s no new request = task loop exited = normal completion →
// session cleaned up (step count already recorded by ErrSchedulerManager).
// In the integration phase: each stop records a step count (no task_start to
// delineate user inputs). Formal version: records the PREVIOUS user input's
// step count when task_start arrives (TODO — jiuwenclaw doesn't send task_start).
func (t *SessionTracker) OnFinish(sessionID string) int {
	if sessionID == "" {
		return 0
	}
	t.mu.Lock()
	defer t.mu.Unlock()

	s, ok := t.sessions[sessionID]
	if !ok {
		log.Warn().Msgf("[ERRSession] OnFinish for unknown session: %s", sessionID)
		return 0
	}

	s.LastActiveTime = time.Now()
	t.armIdleTimer(s, t.taskIdleTimeout) // short timeout: stop = normal completion
	log.Debug().Msgf("[ERRSession] session %s finished, total steps: %d", sessionID, s.Iteration)
	return s.Iteration
}

// OnCancel is called when the HTTP connection is closed abnormally
// (user ESC, new message interrupt, network error).
// The session is removed from tracking but NOT recorded in the distribution
// (abnormally terminated step counts are unreliable).
func (t *SessionTracker) OnCancel(sessionID string) {
	if sessionID == "" {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()

	if s, ok := t.sessions[sessionID]; ok {
		t.cancelIdleTimer(s)
		delete(t.sessions, sessionID)
		log.Debug().Msgf("[ERRSession] session %s cancelled (not recorded)", sessionID)
	}
}

// SetCategory records the CatERR category for a session (classified at OnRequest,
// retrieved at OnFinish to update the per-category step distribution).
func (t *SessionTracker) SetCategory(sessionID, category string) {
	if sessionID == "" || category == "" {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()

	if s, ok := t.sessions[sessionID]; ok {
		s.Category = category
	}
}

// GetCategory returns the CatERR category recorded for a session ("" if unknown).
func (t *SessionTracker) GetCategory(sessionID string) string {
	if sessionID == "" {
		return ""
	}
	t.mu.RLock()
	defer t.mu.RUnlock()

	if s, ok := t.sessions[sessionID]; ok {
		return s.Category
	}
	return ""
}
