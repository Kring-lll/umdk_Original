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
	"sync/atomic"
	"time"

	"huawei.com/aigw/pkg/log"
)

// StreamReader reads Server-Sent Events (SSE) from an HTTP response body.
//
// Architecture: a producer goroutine reads raw lines from the body as they
// arrive and feeds a buffered channel; ReadEvent parses events from that
// channel. The idle watchdog measures actual byte arrival on the PRODUCER
// side, so a consumer stalled by a slow client write can never fire it —
// the two failure domains stay decoupled:
//
//	upstream silent  → watchdog closes the body (dead engine detection)
//	client stalled   → producer keeps reading and buffering; the handler's
//	                   per-write deadline catches it instead
//
// Two tiers, mirroring the client's own stream budgets:
//   first byte:  firstChunkTimeout (default 300s = client
//                stream_first_chunk_timeout; the engine may legitimately
//                queue+prefill for minutes before the first token)
//   byte gap:    chunkIntervalTimeout (default 60s = client
//                stream_idle_timeout; measured inter-chunk gaps are
//                sub-second, so a gap this long means the engine died
//                mid-stream)
// Either tier <= 0 disables that tier; with the gap tier disabled the
// watchdog disarms after the first byte.
type StreamReader struct {
	body    io.ReadCloser
	closed  atomic.Bool
	mu      sync.Mutex // serializes ReadEvent (consumer side only)
	closeCh chan struct{}

	lines       chan string   // producer → consumer, buffered
	done        chan struct{} // closed when the producer exits
	idleReset   chan struct{} // watchdog reset signal (buffered 1)
	firstByteCh chan struct{} // closed when the first data byte arrives

	err   error // terminal producer error, read after done
	errMu sync.Mutex

	label string // for watchdog kill logs (session/req id)
}

// SSEEvent represents a single Server-Sent Event.
type SSEEvent struct {
	Event string // Event name
	Data  string // Event data
	ID    string // Event ID
	Retry int    // Retry interval (ms)
}

// lineBufferSize bounds the producer→consumer channel: large enough that a
// consumer stalled on a client write does not backpressure the producer
// before the stalled write's own deadline fires; small enough to bound
// memory on abandoned consumers.
const lineBufferSize = 4096

// NewStreamReader creates a stream reader with the default idle timeouts.
func NewStreamReader(body io.ReadCloser) *StreamReader {
	return newStreamReader(body, defaultUpstreamFirstChunkTimeout, defaultChunkIntervalTimeout, "")
}

// newStreamReader creates a stream reader whose reads abort after firstIdle
// (no first byte yet) or gapIdle (upstream silence between bytes); <= 0
// disables the respective tier. label appears in watchdog kill logs.
func newStreamReader(body io.ReadCloser, firstIdle, gapIdle time.Duration, label string) *StreamReader {
	sr := &StreamReader{
		body:        body,
		closeCh:     make(chan struct{}),
		lines:       make(chan string, lineBufferSize),
		done:        make(chan struct{}),
		idleReset:   make(chan struct{}, 1),
		firstByteCh: make(chan struct{}),
		label:       label,
	}
	go sr.readLoop(firstIdle, gapIdle)
	return sr
}

// readLoop is the producer: reads lines from the body as they arrive,
// resets the idle watchdog on every actual byte arrival, and delivers lines
// to the consumer channel. Exit ordering (LIFO defers): the consumer
// channel closes first, then the watchdog is told to stop and reaped, then
// done closes for Close().
func (sr *StreamReader) readLoop(firstIdle, gapIdle time.Duration) {
	defer close(sr.done)

	stopCh := make(chan struct{})
	watchdogDone := make(chan struct{})
	if firstIdle > 0 || gapIdle > 0 {
		go func() {
			defer close(watchdogDone)
			sr.idleWatchdog(firstIdle, gapIdle, stopCh)
		}()
	} else {
		close(watchdogDone)
	}
	defer func() {
		close(stopCh)
		<-watchdogDone
	}()

	defer close(sr.lines)

	reader := bufio.NewReader(sr.body)
	for {
		line, err := reader.ReadString('\n')
		if len(line) > 0 {
			sr.markData()
		}
		if err != nil && err != io.EOF {
			// A manual Close unblocks the read as an error; report it as
			// a clean EOF to the consumer.
			if sr.isClosed() {
				sr.setErr(io.EOF)
			} else {
				sr.setErr(err)
			}
			sr.body.Close()
			return
		}
		if len(line) > 0 {
			select {
			case sr.lines <- line:
			case <-sr.closeCh:
				sr.setErr(io.EOF)
				return
			}
		}
		if err == io.EOF {
			sr.setErr(io.EOF)
			return
		}
	}
}

// markData records actual byte arrival: closes firstByteCh on the first
// data and pings the watchdog reset.
func (sr *StreamReader) markData() {
	select {
	case <-sr.firstByteCh:
	default:
		close(sr.firstByteCh)
	}
	select {
	case sr.idleReset <- struct{}{}:
	default:
	}
}

// idleWatchdog closes the body after a full idle interval of no byte
// arrival, unblocking the producer's read as an error. The first tier
// bounds the wait for the first byte; after the first byte the gap tier
// takes over (or the watchdog disarms when the gap tier is disabled).
func (sr *StreamReader) idleWatchdog(firstIdle, gapIdle time.Duration, stopCh chan struct{}) {
	var t *time.Timer
	var timeout <-chan time.Time
	arm := func(d time.Duration) {
		if t == nil {
			t = time.NewTimer(d)
		} else {
			t.Reset(d)
		}
		timeout = t.C
	}
	if firstIdle > 0 {
		arm(firstIdle)
	}
	for {
		if timeout == nil {
			// First tier disabled and no data yet: nothing armed.
			select {
			case <-stopCh:
				return
			case <-sr.idleReset:
				if gapIdle > 0 {
					arm(gapIdle)
				}
			}
			continue
		}
		select {
		case <-stopCh:
			return
		case <-sr.idleReset:
			select {
			case <-sr.firstByteCh:
				if gapIdle > 0 {
					arm(gapIdle)
				} else {
					return // gap tier disabled: disarm after the first byte
				}
			default:
				arm(firstIdle)
			}
		case <-t.C:
			idle := firstIdle
			select {
			case <-sr.firstByteCh:
				idle = gapIdle
			default:
			}
			log.Warn().Msgf("[Proxy] stream silent for %v (label=%s) — closing upstream response (dead upstream?)", idle, sr.label)
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

	event := &SSEEvent{}

	for {
		var line string
		select {
		case l, ok := <-sr.lines:
			if !ok {
				if event.Data != "" || event.Event != "" || event.ID != "" || event.Retry != 0 {
					return event, nil
				}
				err := sr.readErr()
				if err == nil {
					err = io.EOF
				}
				return nil, err
			}
			line = l
		}

		// Remove trailing newline and whitespace
		line = strings.TrimSpace(line)

		// Empty line signals end of event
		if line == "" {
			if event.Data != "" || event.Event != "" || event.ID != "" || event.Retry != 0 {
				return event, nil
			}
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

func (sr *StreamReader) setErr(err error) {
	sr.errMu.Lock()
	sr.err = err
	sr.errMu.Unlock()
}

func (sr *StreamReader) readErr() error {
	sr.errMu.Lock()
	defer sr.errMu.Unlock()
	return sr.err
}

func (sr *StreamReader) isClosed() bool {
	return sr.closed.Load()
}

// Close closes the stream reader and unblocks the producer.
func (sr *StreamReader) Close() error {
	if !sr.closed.CompareAndSwap(false, true) {
		return nil
	}

	close(sr.closeCh)
	err := sr.body.Close()
	<-sr.done
	return err
}

// Closed returns whether the stream is closed.
func (sr *StreamReader) Closed() bool {
	return sr.closed.Load()
}
