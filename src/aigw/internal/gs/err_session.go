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

// ErrSessionState tracks the iteration count and lifecycle of a single user input.
type ErrSessionState struct {
	SessionID      string
	Iteration      int
	Model          string
	Category       string // classified at OnRequest, used at OnFinish
	LastActiveTime time.Time
	idleTimer      *time.Timer
}

// SessionTracker manages session state for all active and completed sessions.
type SessionTracker struct {
	mu              sync.RWMutex
	taskIdleTimeout time.Duration // post-stop idle cleanup (default 60s)
	sessionTimeout  time.Duration // post-tool_calls idle cleanup (default 660s)
	sessions        map[string]*ErrSessionState
}

func NewSessionTracker(taskIdleTimeout, sessionTimeout time.Duration) *SessionTracker {
	return &SessionTracker{
		taskIdleTimeout: taskIdleTimeout,
		sessionTimeout:  sessionTimeout,
		sessions:        make(map[string]*ErrSessionState),
	}
}

// armIdleTimer arms the per-session idle-cleanup timer. Caller must hold t.mu.
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

func (t *SessionTracker) cancelIdleTimer(s *ErrSessionState) {
	if s != nil && s.idleTimer != nil {
		s.idleTimer.Stop()
		s.idleTimer = nil
	}
}

// RestartIdleTimer arms the long idle timer after tool_calls/other finish_reason.
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

// cleanupIdleSession removes the session if it has been inactive for >= timeout.
func (t *SessionTracker) cleanupIdleSession(sessionID string, timeout time.Duration) {
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
	log.Debug().Msgf("[ERRSession] session %s idle-cleanup (no activity for %v)", sessionID, timeout)
}

// OnRequest increments the iteration counter and returns the new k value.
// First request for a session returns k=1.
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
	t.armIdleTimer(s, t.taskIdleTimeout)
	log.Debug().Msgf("[ERRSession] session %s finished, total steps: %d", sessionID, s.Iteration)
	return s.Iteration
}

// OnCancel removes the session. Step count is NOT recorded (unreliable).
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
