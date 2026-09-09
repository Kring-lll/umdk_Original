/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: definitions for messages of GS manager.
 * Create: 2025-06-06
 */

// Package gs is the global scheduler for gateway.
package gs

import (
	"context"
	"sync"
)

// ControlMessage is base struct for control plane message.
type ControlMessage struct {
	Request  interface{}
	Response chan interface{}

	// deliveryMu serializes the result handoff (dispatcher Deliver vs
	// requester Abandon), closing the cancel-vs-dispatch race.
	deliveryMu sync.Mutex
	abandoned  bool
}

// Deliver publishes v to the requester. False means the requester already
// abandoned: v is dropped and the caller must undo any accounting it took.
func (m *ControlMessage) Deliver(v interface{}) bool {
	m.deliveryMu.Lock()
	defer m.deliveryMu.Unlock()
	if m.abandoned {
		return false
	}
	m.Response <- v
	return true
}

// Abandon marks the requester as gone and returns a concurrently published
// result, if any — a dispatched suggestion the caller must release.
func (m *ControlMessage) Abandon() interface{} {
	m.deliveryMu.Lock()
	defer m.deliveryMu.Unlock()
	m.abandoned = true
	select {
	case v := <-m.Response:
		return v
	default:
		return nil
	}
}

// RegisterInstanceMsg is the message used for registering new instance.
type RegisterInstanceMsg struct {
	Name    string
	Model   string
	IP      string
	Port    string
	Role    string
	GroupID string
	DpRank  int // DP rank for data parallel routing
}

// UnregisterInstanceMsg is the message used for unregistering instance.
type UnregisterInstanceMsg struct {
	Model  string
	IP     string
	Port   string
	DpRank int // DP rank for data parallel routing
}

// ScheduleRequestMsg is used to issue schedule request.
type ScheduleRequestMsg struct {
	Request              *LlmRequest
	ReqCTX               context.Context // used to request cancellation
	CandidateInstanceIDs []string        // used for SDK mode restrict scheduling to these instances, if nil means all
	// Fields for consistent hash routing
	Headers map[string]string      // HTTP headers for hash key extraction
	Body    map[string]interface{} // Request body for hash key extraction
	// SessionID for ERR scheduling (extracted from agent_hint.session_id)
	SessionID string
	// Advisory: the caller forwards the request itself, so ERR neither queues it nor takes slots.
	Advisory bool
}

// ExecuteDispatchMsg is used for dispatching schedule request.
type ExecuteDispatchMsg struct {
	Result *ScheduleResult
}

// SuggestionResultMsg is the message return to caller.
type SuggestionResultMsg struct {
	PrefillUrl string
	DecodeUrl  string
	DpRank     *int     // DP rank for data parallel routing
	Priority   *int     // ERR scheduling priority (nil if ERR disabled)
}
