/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: tests for user-prompt extraction helpers (CatERR classification input).
 * Create: 2026-09-07
 */

package server

import (
	"encoding/json"
	"strings"
	"testing"

	"huawei.com/aigw/internal/base"
)

// TestExtractUserText verifies the classification-input chain: system roles
// excluded, framework <system-reminder> wrappers skipped, and the jiuwenswarm
// JSON envelope unwrapped to the real task text.
func TestExtractUserText(t *testing.T) {
	msgs := []base.OpenAiMessage{
		{Role: "system", Content: "You are JiuwenSwarm, an interactive coding agent (34832 chars)"},
		{Role: "user", Content: `你收到一条消息：{"source": "tui", "timezone": "Asia/Shanghai", "content": "帮我回复客户的退款咨询", "type": "user input", "files_updated_by_user": "{}", "trusted_dirs": "[]"}`},
		{Role: "user", Content: "<system-reminder>dynamic context... Current mode: code.normal</system-reminder>"},
	}

	got := extractUserText(msgs)
	if !strings.Contains(got, "帮我回复客户的退款咨询") {
		t.Fatalf("real task text missing: %q", got)
	}
	if strings.Contains(got, "system-reminder") {
		t.Fatalf("framework reminder leaked into classification input: %q", got)
	}
	if strings.Contains(got, "tui") || strings.Contains(got, "trusted_dirs") {
		t.Fatalf("envelope metadata leaked: %q", got)
	}
	if strings.Contains(got, "JiuwenSwarm") {
		t.Fatalf("system prompt leaked: %q", got)
	}
}

func TestUnwrapContentField(t *testing.T) {
	cases := []struct {
		name, in, want string
	}{
		{"plain text unchanged", "just a task", "just a task"},
		{"envelope unwrapped", `你收到一条消息：{"source":"tui","content":"inner task"}`, "inner task"},
		{"json without prefix stays raw", `{"source":"tui","content":"not an envelope"}`, `{"source":"tui","content":"not an envelope"}`},
		{"braces in task text stay raw", `fix the {broken} code in {"path": "/x"}`, `fix the {broken} code in {"path": "/x"}`},
		{"json without content falls back", `你收到一条消息：{"source":"tui","note":"x"}`, `你收到一条消息：{"source":"tui","note":"x"}`},
		{"non-string content falls back", `你收到一条消息：{"content":42}`, `你收到一条消息：{"content":42}`},
		{"empty content falls back", `你收到一条消息：{"content":"  "}`, `你收到一条消息：{"content":"  "}`},
		{"malformed json falls back", `你收到一条消息：{"content": "unterminated`, `你收到一条消息：{"content": "unterminated`},
	}
	for _, c := range cases {
		if got := unwrapContentField(c.in); got != c.want {
			t.Errorf("%s: unwrapContentField(%q) = %q, want %q", c.name, c.in, got, c.want)
		}
	}
}

// TestC27_InjectPriorityPreservesLargeIntegers: json.Number roundtrip must
// not corrupt numbers larger than 2^53 (float64 safe range).
func TestC27_InjectPriorityPreservesLargeIntegers(t *testing.T) {
	// The original formatting (space after colon) must survive the splice.
	body := []byte(`{"trace_id": 9007199254740993, "model": "test"}`)
	result := injectPriority(body, -5)

	if !strings.Contains(string(result), `"trace_id": 9007199254740993`) {
		t.Errorf("large integer corrupted by roundtrip: %s", result)
	}
	if !strings.Contains(string(result), `"priority":-5`) {
		t.Errorf("priority not injected: %s", result)
	}
}

// --- injectPriority byte-splice regression tests ---

// mustInject runs injectPriority and unmarshals the result for assertions.
func mustInject(t *testing.T, body string, priority int) (result string, parsed map[string]interface{}) {
	t.Helper()
	out := injectPriority([]byte(body), priority)
	result = string(out)
	if err := json.Unmarshal(out, &parsed); err != nil {
		t.Fatalf("injected result is not valid JSON: %v\n%s", err, result)
	}
	return result, parsed
}

func TestInjectPriority_ReplacesExisting(t *testing.T) {
	body := `{"model":"test","agent_hint":{"session_id":"s1","priority_control":{"priority":5}},"messages":[{"role":"user","content":"hello"}]}`
	result, parsed := mustInject(t, body, -3)

	pc := parsed["agent_hint"].(map[string]interface{})["priority_control"].(map[string]interface{})
	if pc["priority"].(float64) != -3 {
		t.Errorf("priority = %v, want -3", pc["priority"])
	}
	if parsed["model"] != "test" {
		t.Errorf("model lost: %v", parsed["model"])
	}
	if !strings.Contains(result, `"messages":[{"role":"user","content":"hello"}]`) {
		t.Errorf("messages must be byte-identical, got: %s", result)
	}
}

func TestInjectPriority_PreservesLargeIntegers(t *testing.T) {
	body := `{"trace_id":9007199254740993,"model":"test","agent_hint":{"session_id":"s1"}}`
	result, _ := mustInject(t, body, -5)

	if !strings.Contains(result, `"trace_id":9007199254740993`) {
		t.Errorf("large integer corrupted: %s", result)
	}
}

func TestInjectPriority_PreservesKeyOrder(t *testing.T) {
	body := `{"zebra":"last","apple":"first","agent_hint":{"session_id":"s1"},"banana":"middle"}`
	result, _ := mustInject(t, body, 7)

	// Key order: zebra, apple, agent_hint, banana (original order).
	zi := strings.Index(result, `"zebra"`)
	ai := strings.Index(result, `"apple"`)
	hi := strings.Index(result, `"agent_hint"`)
	bi := strings.Index(result, `"banana"`)
	if !(zi < ai && ai < hi && hi < bi) {
		t.Errorf("key order changed: zebra=%d apple=%d agent_hint=%d banana=%d\n%s", zi, ai, hi, bi, result)
	}
}

func TestInjectPriority_CreatesAgentHintWhenAbsent(t *testing.T) {
	body := `{"model":"test","messages":[{"role":"user","content":"hi"}]}`
	result, parsed := mustInject(t, body, -8)

	pc := parsed["agent_hint"].(map[string]interface{})["priority_control"].(map[string]interface{})
	if pc["priority"].(float64) != -8 {
		t.Errorf("priority = %v, want -8", pc["priority"])
	}
	if !strings.Contains(result, `"model":"test"`) {
		t.Errorf("model lost on insert: %s", result)
	}
}

func TestInjectPriority_CreatesPriorityControlWhenAbsent(t *testing.T) {
	body := `{"model":"test","agent_hint":{"session_id":"s1"}}`
	_, parsed := mustInject(t, body, 42)

	pc := parsed["agent_hint"].(map[string]interface{})["priority_control"].(map[string]interface{})
	if pc["priority"].(float64) != 42 {
		t.Errorf("priority = %v, want 42", pc["priority"])
	}
	// session_id must survive.
	if parsed["agent_hint"].(map[string]interface{})["session_id"] != "s1" {
		t.Errorf("session_id lost")
	}
}

func TestInjectPriority_NestedAgentHintInMessagesNotConfused(t *testing.T) {
	// A message that literally contains "agent_hint" as a property must
	// not confuse the top-level lookup.
	body := `{"messages":[{"role":"user","content":"hi","agent_hint":"nested-string"}],"agent_hint":{"session_id":"real-one"}}`
	_, parsed := mustInject(t, body, -1)

	// The top-level agent_hint must get the priority, not the nested one.
	ah := parsed["agent_hint"].(map[string]interface{})
	if ah["session_id"] != "real-one" {
		t.Errorf("wrong agent_hint targeted: %v", ah)
	}
	pc := ah["priority_control"].(map[string]interface{})
	if pc["priority"].(float64) != -1 {
		t.Errorf("priority = %v, want -1", pc["priority"])
	}
	// The nested agent_hint in messages must be untouched.
	msg := parsed["messages"].([]interface{})[0].(map[string]interface{})
	if msg["agent_hint"] != "nested-string" {
		t.Errorf("nested agent_hint corrupted: %v", msg["agent_hint"])
	}
}

func TestInjectPriority_EmptyObjectInsert(t *testing.T) {
	body := `{}`
	result, parsed := mustInject(t, body, 3)

	pc := parsed["agent_hint"].(map[string]interface{})["priority_control"].(map[string]interface{})
	if pc["priority"].(float64) != 3 {
		t.Errorf("priority = %v, want 3", pc["priority"])
	}
	if result == `{}` {
		t.Errorf("body unchanged after insert")
	}
}

func TestInjectPriority_PreservesUnicode(t *testing.T) {
	body := `{"messages":[{"role":"user","content":"你好世界 🌍"}],"agent_hint":{"session_id":"s1"}}`
	result, _ := mustInject(t, body, -2)

	if !strings.Contains(result, `你好世界 🌍`) {
		t.Errorf("unicode content corrupted: %s", result)
	}
}

func TestInjectPriority_PreservesWhitespaceFormatting(t *testing.T) {
	body := "{\n  \"model\": \"test\",\n  \"custom_field\": 123\n}"
	result, _ := mustInject(t, body, 1)

	// Original formatting of untouched fields must survive byte-identically.
	if !strings.Contains(result, "\"model\": \"test\"") {
		t.Errorf("formatting lost: %s", result)
	}
	if !strings.Contains(result, "\"custom_field\": 123") {
		t.Errorf("custom field formatting lost: %s", result)
	}
}
