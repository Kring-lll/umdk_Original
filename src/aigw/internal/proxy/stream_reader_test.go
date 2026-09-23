/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Unit tests for SSE stream reader.
 * Create: 2026-05-11
 */

package proxy

import (
	"io"
	"strings"
	"testing"
	"time"
)

func TestReadEvent_BasicEvent(t *testing.T) {
	input := "data: Hello World\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Event != "" {
		t.Errorf("expected empty event, got %q", event.Event)
	}
	if event.Data != "Hello World" {
		t.Errorf("expected data 'Hello World', got %q", event.Data)
	}
	if event.ID != "" {
		t.Errorf("expected empty id, got %q", event.ID)
	}
}

func TestReadEvent_EventType(t *testing.T) {
	input := "event: chat.completion.chunk\ndata: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Event != "chat.completion.chunk" {
		t.Errorf("expected event 'chat.completion.chunk', got %q", event.Event)
	}
	expectedData := `{"choices":[{"delta":{"role":"assistant"}}]}`
	if event.Data != expectedData {
		t.Errorf("expected data %q, got %q", expectedData, event.Data)
	}
}

func TestReadEvent_MultiLineData(t *testing.T) {
	input := "data: line1\ndata: line2\ndata: line3\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	expected := "line1\nline2\nline3"
	if event.Data != expected {
		t.Errorf("expected data %q, got %q", expected, event.Data)
	}
}

func TestReadEvent_JsonWithColon(t *testing.T) {
	input := `data: {"content": "hello: world", "url": "http://example.com/path?a=1:b=2"}
`
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	expected := `{"content": "hello: world", "url": "http://example.com/path?a=1:b=2"}`
	if event.Data != expected {
		t.Errorf("expected data %q, got %q", expected, event.Data)
	}
}

func TestReadEvent_RetryField(t *testing.T) {
	input := "retry: 3000\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Retry != 3000 {
		t.Errorf("expected retry 3000, got %d", event.Retry)
	}
}

func TestReadEvent_RetryWithSpaces(t *testing.T) {
	input := "retry:   5000  \n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Retry != 5000 {
		t.Errorf("expected retry 5000, got %d", event.Retry)
	}
}

func TestReadEvent_RetryWithSuffix(t *testing.T) {
	input := "retry: 4000ms\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Retry != 4000 {
		t.Errorf("expected retry 4000, got %d", event.Retry)
	}
}

func TestReadEvent_IdField(t *testing.T) {
	input := "id: 12345\ndata: test\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.ID != "12345" {
		t.Errorf("expected id '12345', got %q", event.ID)
	}
}

func TestReadEvent_NoSpaceAfterColon(t *testing.T) {
	input := "data:no space\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Data != "no space" {
		t.Errorf("expected data 'no space', got %q", event.Data)
	}
}

func TestReadEvent_CommentLine(t *testing.T) {
	input := ": This is a comment\ndata: value\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Data != "value" {
		t.Errorf("expected data 'value', got %q", event.Data)
	}
}

func TestReadEvent_EmptyLinesBetweenEvents(t *testing.T) {
	input := "data: first\n\ndata: second\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event1, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if event1.Data != "first" {
		t.Errorf("expected first event data 'first', got %q", event1.Data)
	}

	event2, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if event2.Data != "second" {
		t.Errorf("expected second event data 'second', got %q", event2.Data)
	}
}

func TestReadEvent_EOFWithData(t *testing.T) {
	input := "data: last event without final newline"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Data != "last event without final newline" {
		t.Errorf("expected data 'last event without final newline', got %q", event.Data)
	}
}

func TestReadEvent_ClosedReader(t *testing.T) {
	sr := NewStreamReader(io.NopCloser(strings.NewReader("")))
	sr.Close()

	event, err := sr.ReadEvent()
	if err != io.EOF {
		t.Errorf("expected io.EOF, got %v", err)
	}
	if event != nil {
		t.Errorf("expected nil event, got %v", event)
	}
}

func TestReadAllEvents(t *testing.T) {
	input := "data: event1\n\nevent: test\nevent: chat.completion.chunk\ndata: {\"delta\":\"a\"}\n\nevent: test\ndata: {\"delta\":\"b\"}\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	events, err := sr.ReadAllEvents()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if len(events) != 3 {
		t.Errorf("expected 3 events, got %d", len(events))
	}

	if events[0].Data != "event1" {
		t.Errorf("expected first event data 'event1', got %q", events[0].Data)
	}
	if events[1].Event != "chat.completion.chunk" {
		t.Errorf("expected second event type 'chat.completion.chunk', got %q", events[1].Event)
	}
	if events[2].Event != "test" || events[2].Data != "{\"delta\":\"b\"}" {
		t.Errorf("expected third event (test, b), got (%q, %q)", events[2].Event, events[2].Data)
	}
}

func TestReadEvent_ComplexJsonData(t *testing.T) {
	json := `{"id":"chatcmpl-123","object":"chat.completion.chunk","created":1677856582,"model":"gpt-3.5-turbo","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}`
	input := "data: " + json + "\n\n"
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	event, err := sr.ReadEvent()
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if event.Data != json {
		t.Errorf("expected data to match input JSON, got %q", event.Data)
	}
}

func TestReadEvent_StreamingChatCompletion(t *testing.T) {
	input := `event: chat.completion.chunk
data: {"id":"chatcmpl-001","object":"chat.completion.chunk","created":1234567890,"model":"gpt-4","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

event: content
data: {"id":"chatcmpl-001","object":"chat.completion.chunk","created":1234567890,"model":"gpt-4","choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]}

event: content
data: {"id":"chatcmpl-001","object":"chat.completion.chunk","created":1234567890,"model":"gpt-4","choices":[{"index":0,"delta":{"content":"!"},"finish_reason":null}]}

event: chat.completion.chunk
data: [DONE]

`
	sr := NewStreamReader(io.NopCloser(strings.NewReader(input)))

	count := 0
	for {
		_, err := sr.ReadEvent()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		count++
	}

	if count != 4 {
		t.Errorf("expected 4 events, got %d", count)
	}
}

func TestClosed(t *testing.T) {
	sr := NewStreamReader(io.NopCloser(strings.NewReader("")))

	if sr.Closed() {
		t.Error("expected Closed() to return false before Close()")
	}

	sr.Close()

	if !sr.Closed() {
		t.Error("expected Closed() to return true after Close()")
	}

	// Double close should not panic
	sr.Close()
}

func TestReadEvent_VariousFieldFormats(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected string
		field    string
	}{
		{"colon_space", "data: value\n\n", "value", "Data"},
		{"colon_only", "data:value\n\n", "value", "Data"},
		{"colon_space_comment", ": comment\ndata: value\n\n", "value", "Data"},
		{"event_colon_space", "event: myevent\n\n", "myevent", "Event"},
		{"id_colon_space", "id: abc123\ndata: x\n\n", "abc123", "ID"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			sr := NewStreamReader(io.NopCloser(strings.NewReader(tt.input)))
			event, err := sr.ReadEvent()
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}

			switch tt.field {
			case "Data":
				if event.Data != tt.expected {
					t.Errorf("expected Data %q, got %q", tt.expected, event.Data)
				}
			case "Event":
				if event.Event != tt.expected {
					t.Errorf("expected Event %q, got %q", tt.expected, event.Event)
				}
			case "ID":
				if event.ID != tt.expected {
					t.Errorf("expected ID %q, got %q", tt.expected, event.ID)
				}
			}
		})
	}
}

// TestStreamReaderIdleWatchdogDeadUpstream: a body that never delivers any
// byte (dead engine / wedged coordinator) must unblock ReadEvent within the
// idle timeout instead of hanging until Client.Timeout or the zombie scan —
// the cal_deadlock forensic showed handlers stuck for 17+ minutes holding
// every in-flight slot after an engine crash.
func TestStreamReaderIdleWatchdogDeadUpstream(t *testing.T) {
	pr, _ := io.Pipe() // never written, never closed
	defer pr.Close()

	sr := newStreamReader(pr, 150*time.Millisecond, 150*time.Millisecond, "")
	defer sr.Close()

	done := make(chan error, 1)
	go func() {
		_, err := sr.ReadEvent()
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil || err == io.EOF {
			t.Fatalf("expected a read error from the closed body, got %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("ReadEvent still blocked after the idle timeout — dead upstream hangs the handler")
	}
}

// TestStreamReaderIdleWatchdogResetByData: chunks arriving within the idle
// interval keep the stream alive; the watchdog must not fire on a healthy
// slow stream.
func TestStreamReaderIdleWatchdogResetByData(t *testing.T) {
	pr, pw := io.Pipe()
	defer pr.Close()
	sr := newStreamReader(pr, time.Hour, 120*time.Millisecond, "")
	defer sr.Close()

	go func() {
		for i := 0; i < 3; i++ {
			pw.Write([]byte("data: chunk\n\n"))
			time.Sleep(60 * time.Millisecond) // half the idle interval
		}
		pw.Close()
	}()

	for i := 0; i < 3; i++ {
		ev, err := sr.ReadEvent()
		if err != nil {
			t.Fatalf("event %d: unexpected error %v (watchdog fired on a live stream)", i, err)
		}
		if ev.Data != "chunk" {
			t.Fatalf("event %d: got %q", i, ev.Data)
		}
	}
	// Then EOF on close.
	if _, err := sr.ReadEvent(); err != io.EOF {
		t.Fatalf("expected EOF after writer close, got %v", err)
	}
}


// TestReadEventSurfacesReadError: a non-EOF read error (closed body,
// connection reset) must surface to the caller — the old code looped on it
// forever, busy-spinning under the reader lock.
func TestReadEventSurfacesReadError(t *testing.T) {
	pr, _ := io.Pipe()
	pr.Close() // reader side closed: subsequent reads return ErrClosedPipe
	sr := newStreamReader(pr, time.Hour, time.Hour, "")
	defer sr.Close()
	if _, err := sr.ReadEvent(); err == nil || err == io.EOF {
		t.Fatalf("expected a read error, got %v", err)
	}
}

// TestStreamReaderWatchdogDisabled: idle <= 0 disables the watchdog — a
// silent upstream blocks (legacy behavior for negative config).
func TestStreamReaderWatchdogDisabled(t *testing.T) {
	pr, _ := io.Pipe()
	defer pr.Close()
	sr := newStreamReader(pr, -1, -1, "")
	// No sr.Close() here: it takes the reader lock that the blocked
	// ReadEvent below holds forever — with the watchdog disabled nothing
	// can unblock it, so closing would deadlock the test.

	done := make(chan error, 1)
	go func() {
		_, err := sr.ReadEvent()
		done <- err
	}()
	select {
	case <-done:
		t.Fatal("watchdog disabled must leave the read blocked")
	case <-time.After(200 * time.Millisecond):
	}
}

// TestStreamReaderTwoTierGapAfterFirstByte: once the first byte has arrived,
// silence is judged by the (shorter) gap tier, not the first-chunk tier —
// an engine that dies mid-stream must not hold the slot for the full
// first-chunk budget.
func TestStreamReaderTwoTierGapAfterFirstByte(t *testing.T) {
	pr, pw := io.Pipe()
	defer pr.Close()
	// First chunk arrives, then silence forever.
	go func() {
		pw.Write([]byte("data: first\n\n"))
	}()
	sr := newStreamReader(pr, time.Hour, 200*time.Millisecond, "")
	defer sr.Close()

	if ev, err := sr.ReadEvent(); err != nil || ev.Data != "first" {
		t.Fatalf("first event: %v %v", ev, err)
	}

	done := make(chan error, 1)
	go func() {
		_, err := sr.ReadEvent()
		done <- err
	}()
	select {
	case err := <-done:
		if err == nil || err == io.EOF {
			t.Fatalf("expected read error from watchdog, got %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("gap tier did not fire — mid-stream death holds the slot for the first-chunk budget")
	}
}

// TestStreamReaderConsumerStallDoesNotMisfire: THE misfire regression — a
// consumer stalled (e.g. blocked writing to a slow client) must not fire
// the read watchdog while the upstream keeps streaming. The old
// implementation reset the watchdog at ReadEvent entry, so the consumer's
// pace was the clock: under real load (24 sessions, GIL pauses) healthy
// streams were killed at exactly the gap timeout. The watchdog now
// measures actual byte arrival on the producer side.
func TestStreamReaderConsumerStallDoesNotMisfire(t *testing.T) {
	pr, pw := io.Pipe()
	defer pr.Close()
	// Gap far shorter than the consumer stall below: any consumer-paced
	// measurement would fire; only producer-side measurement survives.
	sr := newStreamReader(pr, 200*time.Millisecond, 200*time.Millisecond, "")
	defer sr.Close()

	go func() {
		for i := 0; i < 20; i++ {
			pw.Write([]byte("data: chunk\n\n"))
			time.Sleep(50 * time.Millisecond)
		}
		pw.Close()
	}()

	// Consumer stalls 600ms = 3x the gap timeout (simulated blocked write).
	time.Sleep(600 * time.Millisecond)

	for i := 0; i < 20; i++ {
		if _, err := sr.ReadEvent(); err != nil {
			t.Fatalf("misfire: consumer stall killed a healthy upstream stream: %v", err)
		}
	}
	if _, err := sr.ReadEvent(); err != io.EOF {
		t.Fatalf("expected EOF after writer close, got %v", err)
	}
}

// TestStreamReaderGapDisabledDisarmsAfterFirstByte: with the gap tier
// disabled (<= 0), the first-chunk tier must disarm once the first byte
// arrives — the old code kept it armed for the whole stream life, making
// 300/-1 kill 91% of streams at 300s of inter-READ silence.
func TestStreamReaderGapDisabledDisarmsAfterFirstByte(t *testing.T) {
	pr, pw := io.Pipe()
	go pw.Write([]byte("data: first\n\n"))
	sr := newStreamReader(pr, 10*time.Hour, -1, "")
	defer sr.Close()

	if ev, err := sr.ReadEvent(); err != nil || ev.Data != "first" {
		t.Fatalf("first event: %v %v", ev, err)
	}

	// Upstream goes silent; the reader must stay alive (watchdog disarmed).
	done := make(chan error, 1)
	go func() {
		_, err := sr.ReadEvent()
		done <- err
	}()
	select {
	case err := <-done:
		t.Fatalf("stream killed after first byte with gap disabled: %v", err)
	case <-time.After(400 * time.Millisecond):
		// still blocked = alive
	}
}

// TestStreamReaderLongConsumerStallWithSlowChunks: deployment-shaped variant
// of the misfire regression — seconds-scale gap timeout, tens-of-seconds
// consumer stall, upstream chunking at realistic SSE pacing (one chunk per
// ~2s, as motor delivers). The producer must keep resetting the watchdog
// through the entire stall.
func TestStreamReaderLongConsumerStallWithSlowChunks(t *testing.T) {
	if testing.Short() {
		t.Skip("long timing test")
	}
	pr, pw := io.Pipe()
	defer pr.Close()
	sr := newStreamReader(pr, 3*time.Second, 3*time.Second, "")
	defer sr.Close()

	go func() {
		for i := 0; i < 15; i++ {
			pw.Write([]byte("data: chunk\n\n"))
			time.Sleep(2 * time.Second)
		}
		pw.Close()
	}()

	// Consumer stalls 9s (3x the gap) mid-stream, then resumes.
	time.Sleep(9 * time.Second)
	for i := 0; i < 15; i++ {
		if _, err := sr.ReadEvent(); err != nil {
			t.Fatalf("misfire at deployment scale: %v", err)
		}
	}
}

// TestStreamReaderConcurrentTorture: concurrent Close + watchdog fire +
// consumer reads exercise every shutdown interleaving. Race detector
// verifies no data race; functional assertion is "everything terminates
// without deadlock".
func TestStreamReaderConcurrentTorture(t *testing.T) {
	done := make(chan struct{})
	defer close(done)

	for i := 0; i < 20; i++ {
		pr, pw := io.Pipe()
		sr := newStreamReader(pr, 50*time.Millisecond, 50*time.Millisecond, "torture")

		// Writer: floods then abandons (no Close — producer must handle
		// watchdog-killed body and consumer-driven shutdown alike).
		go func() {
			defer pw.Close()
			for j := 0; j < 100; j++ {
				select {
				case <-done:
					return
				default:
				}
				if _, err := pw.Write([]byte("data: x\n\n")); err != nil {
					return
				}
				time.Sleep(10 * time.Millisecond)
			}
		}()

		// Consumer: reads some events then Closes mid-stream (handler-exit
		// shape — Close while the producer is mid-send).
		go func(sr *StreamReader) {
			deadline := time.After(3 * time.Second)
			for {
				select {
				case <-deadline:
					sr.Close()
					return
				default:
				}
				if _, err := sr.ReadEvent(); err != nil {
					sr.Close()
					return
				}
			}
		}(sr)

		// Close from another goroutine at an arbitrary moment too.
		go func(sr *StreamReader) {
			time.Sleep(time.Duration(i*10) * time.Millisecond)
			sr.Close()
		}(sr)
	}
	time.Sleep(2 * time.Second)
}
