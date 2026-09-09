/*
 * SPDX-License-Identifier: MIT
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

// ErrSessionState tracks the iteration count and lifecycle of a single session.
type ErrSessionState struct {
	SessionID      string
	Iteration      int
	Model          string
	Category       string
	LastActiveTime time.Time
	idleTimer      *time.Timer

	// QueueWaitCarry: queue wait that never earned a dispatch, carried
	// into the next attempt's starvation clock. Consumed on enqueue.
	QueueWaitCarry time.Duration
}

// SessionTracker manages per-session state for ERR scheduling.
type SessionTracker struct {
	mu              sync.RWMutex
	taskIdleTimeout time.Duration
	sessionTimeout  time.Duration
	sessions        map[string]*ErrSessionState
}

func NewSessionTracker(taskIdleTimeout, sessionTimeout time.Duration) *SessionTracker {
	return &SessionTracker{
		taskIdleTimeout: taskIdleTimeout,
		sessionTimeout:  sessionTimeout,
		sessions:        make(map[string]*ErrSessionState),
	}
}

func (t *SessionTracker) armIdleTimer(s *ErrSessionState, timeout time.Duration, isLong bool) {
	if timeout == 0 || s == nil {
		return
	}
	if s.idleTimer != nil {
		s.idleTimer.Stop()
	}
	sessionID := s.SessionID
	s.idleTimer = time.AfterFunc(timeout, func() {
		t.cleanupIdleSession(sessionID, timeout, isLong)
	})
}

func (t *SessionTracker) cancelIdleTimer(s *ErrSessionState) {
	if s != nil && s.idleTimer != nil {
		s.idleTimer.Stop()
		s.idleTimer = nil
	}
}

func (t *SessionTracker) RestartIdleTimer(sessionID string) {
	if sessionID == "" || t.sessionTimeout == 0 {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	if s, ok := t.sessions[sessionID]; ok {
		s.LastActiveTime = time.Now()
		t.armIdleTimer(s, t.sessionTimeout, true)
	}
}

func (t *SessionTracker) cleanupIdleSession(sessionID string, timeout time.Duration, isLong bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	s, ok := t.sessions[sessionID]
	if !ok {
		return
	}
	if time.Since(s.LastActiveTime) < timeout {
		return
	}
	if s.idleTimer != nil {
		s.idleTimer.Stop()
	}
	delete(t.sessions, sessionID)
	if isLong {
		log.Warn().Msgf("[ERRSession] session %s idle-cleanup after %v (agent likely disconnected after tool_calls)", sessionID, timeout)
	}
}

// OnRequest increments the iteration counter and returns k. First request returns k=1.
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
		return 1
	}

	s.Iteration++
	s.LastActiveTime = now
	s.Model = model
	t.cancelIdleTimer(s)
	return s.Iteration
}

func (t *SessionTracker) GetIteration(sessionID string) int {
	if sessionID == "" {
		return 0
	}
	t.mu.RLock()
	defer t.mu.RUnlock()
	if s, ok := t.sessions[sessionID]; ok {
		return s.Iteration
	}
	return 0
}

// RollbackRequest reverts one OnRequest increment after a queued request
// timed out before dispatch: the failed attempt must not count as a step.
// The session survives so the retry re-enters at the same k with its earned
// priority. Arms the long idle timer so a session whose client gave up
// entirely is still cleaned up.
func (t *SessionTracker) RollbackRequest(sessionID string, waited time.Duration) {
	if sessionID == "" {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	s, ok := t.sessions[sessionID]
	if !ok {
		return
	}
	if s.Iteration > 1 {
		s.Iteration--
	}
	s.QueueWaitCarry += waited
	s.LastActiveTime = time.Now()
	t.armIdleTimer(s, t.sessionTimeout, true)
}

// TakeQueueWaitCarry returns and resets the session's carried wait debt.
func (t *SessionTracker) TakeQueueWaitCarry(sessionID string) time.Duration {
	if sessionID == "" {
		return 0
	}
	t.mu.Lock()
	defer t.mu.Unlock()
	s, ok := t.sessions[sessionID]
	if !ok {
		return 0
	}
	carry := s.QueueWaitCarry
	s.QueueWaitCarry = 0
	return carry
}

// OnFinish returns the total step count and arms the short idle timer.
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
	t.armIdleTimer(s, t.taskIdleTimeout, false)
	return s.Iteration
}

// OnCancel removes the session without recording its step count.
func (t *SessionTracker) OnCancel(sessionID string) {
	if sessionID == "" {
		return
	}
	t.mu.Lock()
	defer t.mu.Unlock()

	if s, ok := t.sessions[sessionID]; ok {
		t.cancelIdleTimer(s)
		delete(t.sessions, sessionID)
	}
}

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
