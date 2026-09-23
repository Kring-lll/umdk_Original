/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: tests for user-prompt extraction helpers (CatERR classification input).
 * Create: 2026-09-07
 */

package server

import (
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
		{"json without content falls back", `{"source":"tui","note":"x"}`, `{"source":"tui","note":"x"}`},
		{"non-string content falls back", `{"content":42}`, `{"content":42}`},
		{"empty content falls back", `{"content":"  "}`, `{"content":"  "}`},
		{"malformed json falls back", `{"content": "unterminated`, `{"content": "unterminated`},
	}
	for _, c := range cases {
		if got := unwrapContentField(c.in); got != c.want {
			t.Errorf("%s: unwrapContentField(%q) = %q, want %q", c.name, c.in, got, c.want)
		}
	}
}
