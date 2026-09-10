/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: SSE stream reader for AIGW proxy.
 * Create: 2026-04-29
 */

// Package proxy provides HTTP request proxying for AIGW.
package proxy

import (
	"bufio"
	"io"
	"strings"
	"sync"
	"time"

	"huawei.com/aigw/pkg/log"
)

// StreamReader reads Server-Sent Events (SSE) from an HTTP response body.
type StreamReader struct {
	reader  *bufio.Reader
	body    io.ReadCloser
	closed  bool
	mu      sync.Mutex
	closeCh chan struct{}

	idleReset    chan struct{} // watchdog reset signal (buffered 1)
	firstByteCh  chan struct{} // closed when the first data byte arrives
	watchdogDone chan struct{}
}

// Two read-idle tiers, mirroring the client's own stream timeouts so the
// slot frees roughly when the last possible reader gives up:
//   first byte:  defaultUpstreamFirstChunkTimeout (300s = client
//                stream_first_chunk_timeout; the engine may legitimately
//                queue+prefill for minutes before the first token)
//   byte gap:    defaultUpstreamChunkGapTimeout (60s = client
//                stream_idle_timeout; measured inter-chunk gaps are
//                sub-second, so a 60s gap means the engine died mid-stream)
var (
	defaultUpstreamFirstChunkTimeout = 300 * time.Second
	defaultUpstreamChunkGapTimeout   = 60 * time.Second
)

// SSEEvent represents a single Server-Sent Event.
type SSEEvent struct {
	Event string // Event name
	Data  string // Event data
	ID    string // Event ID
	Retry int    // Retry interval (ms)
}

// NewStreamReader creates a stream reader with the default idle timeouts.
func NewStreamReader(body io.ReadCloser) *StreamReader {
	return newStreamReader(body, defaultUpstreamFirstChunkTimeout, defaultUpstreamChunkGapTimeout)
}

// newStreamReader creates a stream reader whose reads abort after firstIdle
// (no first byte yet) or gapIdle (silence between bytes); <= 0 disables the
// respective tier.
func newStreamReader(body io.ReadCloser, firstIdle, gapIdle time.Duration) *StreamReader {
	sr := &StreamReader{
		reader:       bufio.NewReader(body),
		body:         body,
		closeCh:      make(chan struct{}),
		idleReset:    make(chan struct{}, 1),
		firstByteCh:  make(chan struct{}),
		watchdogDone: make(chan struct{}),
	}
	if firstIdle > 0 || gapIdle > 0 {
		go sr.idleWatchdog(firstIdle, gapIdle)
	}
	return sr
}

// idleWatchdog closes the body after a full idle interval, unblocking the
// pending ReadEvent as a read-on-closed-body error. The first tier bounds
// the wait for the first byte; once data has arrived, later resets use the
// shorter gap tier.
func (sr *StreamReader) idleWatchdog(firstIdle, gapIdle time.Duration) {
	defer close(sr.watchdogDone)
	idle := firstIdle
	if idle <= 0 {
		idle = gapIdle
	}
	if idle <= 0 {
		return
	}
	t := time.NewTimer(idle)
	defer t.Stop()
	for {
		select {
		case <-sr.closeCh:
			return
		case <-sr.idleReset:
			// A reset carrying data means the first byte arrived: tighten
			// to the gap tier from now on.
			select {
			case <-sr.firstByteCh:
				if gapIdle > 0 {
					idle = gapIdle
				}
			default:
			}
			t.Reset(idle)
		case <-t.C:
			log.Warn().Msgf("[Proxy] stream silent for %v (dead upstream?) — closing response body", idle)
			sr.body.Close()
			return
		}
	}
}

// ReadEvent reads the next SSE event from the stream.
// Returns io.EOF when the stream is closed.
func (sr *StreamReader) ReadEvent() (*SSEEvent, error) {
	sr.mu.Lock()
	defer sr.mu.Unlock()

	if sr.closed {
		return nil, io.EOF
	}

	select {
	case sr.idleReset <- struct{}{}:
	default:
	}
	if line, err := sr.reader.Peek(1); err == nil && len(line) > 0 {
		select {
		case <-sr.firstByteCh:
		default:
			close(sr.firstByteCh)
		}
	}

	event := &SSEEvent{}

	for {
		line, err := sr.reader.ReadString('\n')

		// Non-EOF read errors (closed body, connection reset) must surface:
		// looping would busy-spin holding the reader lock forever.
		if err != nil && err != io.EOF {
			return nil, err
		}

		// If EOF with no content read, return EOF (unless we have accumulated event data)
		if err == io.EOF && line == "" {
			if event.Data != "" || event.Event != "" || event.ID != "" || event.Retry != 0 {
				return event, nil
			}
			return nil, io.EOF
		}

		// Remove trailing newline and whitespace
		line = strings.TrimSpace(line)

		// Empty line signals end of event
		if line == "" {
			// Return event if we have any data (including multi-line data)
			if event.Data != "" || event.Event != "" || event.ID != "" || event.Retry != 0 {
				return event, nil
			}
			// No data accumulated yet - continue reading (or EOF will be handled in next iteration)
			continue
		}

		// Parse SSE field
		var fieldName, fieldValue string

		// SSE spec: field name is case-insensitive, value comes after ": " (preferred) or ":"
		// Only split on first colon, value part keeps all content including any colons
		if idx := strings.IndexByte(line, ':'); idx >= 0 {
			fieldName = strings.TrimSpace(line[:idx])
			fieldValue = line[idx+1:]
			// If value starts with space, trim it (standard format is ": ")
			if len(fieldValue) > 0 && fieldValue[0] == ' ' {
				fieldValue = fieldValue[1:]
			}
		} else {
			// No colon found, could be a comment (starts with :) or invalid line
			if strings.HasPrefix(line, ":") {
				continue // comment line, ignore
			}
			continue // invalid line
		}

		switch fieldName {
		case "event":
			event.Event = fieldValue
		case "data":
			if event.Data != "" {
				event.Data += "\n"
			}
			event.Data += fieldValue
		case "id":
			event.ID = fieldValue
		case "retry":
			// Parse retry interval - trim first, then extract numeric portion
			fieldValue = strings.TrimSpace(fieldValue)
			for _, c := range fieldValue {
				if c >= '0' && c <= '9' {
					event.Retry = event.Retry*10 + int(c-'0')
				} else {
					break
				}
			}
		}

		// After processing a field, check if we're at EOF
		if err == io.EOF {
			return event, nil
		}
	}
}

// ReadAllEvents reads all events from the stream until EOF.
// Returns a slice of all events read.
func (sr *StreamReader) ReadAllEvents() ([]*SSEEvent, error) {
	var events []*SSEEvent
	for {
		event, err := sr.ReadEvent()
		if err != nil {
			if err == io.EOF {
				return events, nil
			}
			return events, err
		}
		events = append(events, event)
	}
}

// Close closes the stream reader.
func (sr *StreamReader) Close() error {
	sr.mu.Lock()
	defer sr.mu.Unlock()

	if sr.closed {
		return nil
	}

	sr.closed = true
	close(sr.closeCh)
	if sr.body != nil {
		return sr.body.Close()
	}
	return nil
}

// Closed returns whether the stream is closed.
func (sr *StreamReader) Closed() bool {
	sr.mu.Lock()
	defer sr.mu.Unlock()
	return sr.closed
}
