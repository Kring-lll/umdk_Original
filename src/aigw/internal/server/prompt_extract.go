/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: User-prompt extraction feeding ERR/CatERR classification.
 * Create: 2026-09-14
 */

package server

import (
	"encoding/json"
	"strings"

	"huawei.com/aigw/internal/base"
)

// internalUserPrefixes: SDK-injected user-role payloads that must not feed
// the classification input. Mirrors openjiuwen INTERNAL_USER_PREFIXES —
// keep in sync when the SDK list evolves.
var internalUserPrefixes = []string{
	"<system-reminder", "<memory_block_current", "<memory_block_dialogue",
	"<memory_block_round", "<memory_block_session", "<recovered_context",
	"[STATE_REINJECTION",
}

func isInternalUserMessage(content string) bool {
	for _, p := range internalUserPrefixes {
		if strings.HasPrefix(content, p) {
			return true
		}
	}
	return false
}

// extractUserText returns the concatenated user-role message contents,
// skipping framework-injected wrappers. It feeds ERR/CatERR classification
// so the TF-IDF vector reflects the user's task text instead of being
// dominated by large agent system prompts.
func extractUserText(messages []base.OpenAiMessage) string {
	userText := ""
	for _, m := range messages {
		if m.Role != "user" {
			continue
		}
		c := strings.TrimSpace(m.Content)
		if c == "" || isInternalUserMessage(c) {
			continue
		}
		userText += unwrapContentField(c) + " "
	}
	return userText
}

// jiuwenEnvelopePrefix is the fixed prefix of jiuwenswarm's channel envelope;
// it doubles as the upstream identity check — other agents' messages never
// carry it and are returned verbatim.
const jiuwenEnvelopePrefix = "你收到一条消息："

// unwrapContentField extracts the inner "content" string when the user message
// embeds a jiuwenswarm JSON envelope. The envelope's metadata fields
// (source/files/trusted_dirs/...) are classification noise that can flip the
// category. Only the known envelope prefix gates the JSON parse — arbitrary
// task text containing {...} fragments must not be reinterpreted. Falls back
// to the raw text when no parseable object with a string "content" is found.
func unwrapContentField(s string) string {
	if !strings.HasPrefix(s, jiuwenEnvelopePrefix) {
		return s
	}
	i := strings.Index(s, "{")
	if i < 0 {
		return s
	}
	j := strings.LastIndex(s, "}")
	if j <= i {
		return s
	}
	var obj map[string]interface{}
	if err := json.Unmarshal([]byte(s[i:j+1]), &obj); err != nil {
		return s
	}
	if c, ok := obj["content"].(string); ok && strings.TrimSpace(c) != "" {
		return c
	}
	return s
}
