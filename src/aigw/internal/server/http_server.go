/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: HttpServer provides HTTP services for AIGW.
 * Create: 2025-05-13
 */

// Package server provides north interfaces for AIGW.
package server

import "C"
import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"huawei.com/aigw/internal/apipool"
	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/core"
	"huawei.com/aigw/internal/gs"
	"huawei.com/aigw/internal/proxy"
	"huawei.com/aigw/pkg/crypto"
	"huawei.com/aigw/pkg/log"
)

const (
	shutDownTimeout        = 3 * time.Second
	waitServerReadyTimeout = 3 * time.Second
	waitServerReadyDelay   = 300 * time.Millisecond
	httpTimeout            = 50 * time.Millisecond
)

var maxConcurrency = 128

// HttpServer provides inference suggestion service.
type HttpServer struct {
	manager *core.AigwManager
	server  *http.Server
	host    string
	port    string

	serHmacMgr *crypto.HmacManager
	serAesMgr  *crypto.AesManager

	isReady bool
	readyMu sync.RWMutex

	// Proxy manager for request forwarding
	proxyMgr *proxy.ProxyManager
}

// NewHttpServer creates a new httpServer manager.
func NewHttpServer(manager *core.AigwManager, host string, port string, proxyMgr *proxy.ProxyManager) *HttpServer {
	return &HttpServer{
		manager:    manager,
		host:       host,
		port:       port,
		serHmacMgr: crypto.NewHmacManager(nil),
		serAesMgr:  crypto.NewAesManager(nil),
		isReady:    false,
		proxyMgr:   proxyMgr,
	}
}

func (s *HttpServer) setReady() {
	s.readyMu.Lock()
	defer s.readyMu.Unlock()
	s.isReady = true

}

func (s *HttpServer) isServerReady() bool {
	s.readyMu.RLock()
	defer s.readyMu.RUnlock()
	return s.isReady
}

// Start the httpServer.
func (s *HttpServer) Start() error {
	reqChan := make(chan struct{}, maxConcurrency)

	// Global Concurrency Limit Middleware
	limiterMiddleware := func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/aigw/v1/health" {
				next.ServeHTTP(w, r)
				return
			}

			if !s.isServerReady() {
				http.Error(w, "Service Not Ready", http.StatusServiceUnavailable)
				return
			}
			select {
			case reqChan <- struct{}{}:
				defer func() { <-reqChan }()
				next.ServeHTTP(w, r) // Proceed to the subsequent middleware or handler
			default:
				msg := fmt.Sprintf("Too many requests; the current number of requests is %v, "+
					"and the maximum number of requests is %v.", maxConcurrency, maxConcurrency)
				log.Error().Msg(msg)
				http.Error(w, msg, http.StatusTooManyRequests)
			}

		})
	}

	mx := http.NewServeMux()
	s.registerRoutes(mx)
	s.server = &http.Server{
		Addr:    net.JoinHostPort(s.host, s.port),
		Handler: limiterMiddleware(mx),
	}

	log.Info().Msgf("HTTP server starting on %v", s.server.Addr)

	errChan := make(chan error, 1)
	go func() {
		var err error
		if err = s.server.ListenAndServe(); err != nil {
			log.Warn().Msgf("server exited abnormally, err: %v", err)
		}
		errChan <- err
	}()

	// Wait for the server to be ready
	return s.checkHealth(errChan)
}

// registerRoutes wires all ServeMux routes. Called from Start(); also used by tests
// (via testHandler) to exercise handlers without the limiter middleware / network bind.
func (s *HttpServer) registerRoutes(mx *http.ServeMux) {
	mx.HandleFunc("/aigw/v1/health", s.health)
	mx.HandleFunc("/aigw/v1/register-instance", s.serHmacMgr.WithHMAC(s.registerInstance))
	mx.HandleFunc("/aigw/v1/unregister-instance", s.serHmacMgr.WithHMAC(s.unregisterInstance))
	mx.HandleFunc("/aigw/v1/openai/get-suggestion",
		s.serHmacMgr.WithHMAC(s.serAesMgr.WithAesDecrypt(s.scheduleForOpenAi)))
	mx.HandleFunc("/aigw/v1/stats", s.serHmacMgr.WithHMAC(s.stats))
	// KVC agent lifecycle + debug endpoints (Phase 2; ServiceMode only). Registered only when
	// KVC management is enabled (s.manager.GetAgentRegistry() != nil).
	if s.manager.GetAgentRegistry() != nil {
		mx.HandleFunc("/aigw/v1/agents/register", s.serHmacMgr.WithHMAC(s.agentRegister))
		mx.HandleFunc("/aigw/v1/agents", s.serHmacMgr.WithHMAC(s.agentsList))     // GET list (debug)
		mx.HandleFunc("/aigw/v1/agents/", s.serHmacMgr.WithHMAC(s.agentRoute))    // catch-all sub-router for /agents/{id}/*
		mx.HandleFunc("/aigw/v1/models/", s.serHmacMgr.WithHMAC(s.modelKvcRoute)) // debug: /models/{m}/kvc/...
		log.Info().Msg("KVC agent lifecycle + debug endpoints enabled: /aigw/v1/agents/*, /aigw/v1/models/*/kvc/*")
	}
	// Forwarding endpoint for chat completions (proxy mode)
	if s.proxyMgr != nil {
		mx.HandleFunc("/aigw/v1/openai/chat/completions",
			s.serHmacMgr.WithHMAC(s.serAesMgr.WithAesDecrypt(s.forwardChatCompletions)))
		log.Info().Msg("Proxy forwarding endpoint enabled: /aigw/v1/openai/chat/completions")
	}
}

// testHandler returns a ServeMux with all routes registered, without the limiter
// middleware or network bind — for httptest.NewRecorder-based handler tests.
// KVC agent endpoints are registered WITHOUT the HMAC wrapper so tests can call them
// unsigned (production wiring keeps the HMAC wrapper via registerRoutes).
func (s *HttpServer) testHandler() http.Handler {
	mx := http.NewServeMux()
	mx.HandleFunc("/aigw/v1/health", s.health)
	mx.HandleFunc("/aigw/v1/stats", s.stats)
	if s.manager.GetAgentRegistry() != nil {
		mx.HandleFunc("/aigw/v1/agents/register", s.agentRegister)
		mx.HandleFunc("/aigw/v1/agents", s.agentsList)
		mx.HandleFunc("/aigw/v1/agents/", s.agentRoute)
		mx.HandleFunc("/aigw/v1/models/", s.modelKvcRoute)
	}
	return mx
}

// Stop the httpServer.
func (s *HttpServer) Stop() {
	if s.server == nil {
		return
	}

	log.Info().Msg("shutting down HTTP server")

	ctx, _ := context.WithTimeout(context.Background(), shutDownTimeout)
	if err := s.server.Shutdown(ctx); err != nil {
		log.Warn().Msgf("HTTP server stop failed: %v", err)
		return
	}

	log.Info().Msg("HTTP server shutdown gracefully")
}

// health will check the health of AIGW, return 200 when healthy, else value when not healthy
func (s *HttpServer) health(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}
	log.Debug().Msgf("AIGW is health")
	w.WriteHeader(http.StatusOK)
}

// stats will check the statistical counts of the aigw in data plane.
func (s *HttpServer) stats(w http.ResponseWriter, r *http.Request) {
	if r.Method != "GET" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	result := s.manager.GetAllStats()
	jsonData, err := json.Marshal(result)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)

	if _, err = w.Write(jsonData); err != nil {
		log.Warn().Msgf("Error writing response: %v", err)
	}
}

// registerInstance is the north interface to register new instance for globalScheduler.
// Notice: this function is optional, only used for testing.
func (s *HttpServer) registerInstance(w http.ResponseWriter, r *http.Request) {
	if s.manager.IsEnableZK() {
		err := fmt.Errorf("the zookeeper manager is enable, please use zookeeper to register or unregister")
		http.Error(w, err.Error(), http.StatusForbidden)
		log.Error().Msgf("%v", err)
		return
	}

	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if r.Body == nil || r.ContentLength == 0 {
		err := fmt.Errorf("the body is None")
		http.Error(w, err.Error(), http.StatusBadRequest)
		log.Error().Msgf("failed to decode register instance, err: %v", err)
		return
	}

	var req base.RegisterInstanceIn
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		log.Error().Msgf("failed to decode register instance, err: %v", err)
		return
	}

	log.Info().Msgf("start to register instance %v, model: %v, role: %v", req.Name, req.Model, req.Role)

	if err := s.manager.RegisterInstance(&req); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		log.Error().Msgf("failed to register instance, err: %v", err)
		return
	}

	w.WriteHeader(http.StatusOK)
	log.Info().Msgf("register instance successfully, model: %v, role: %v", req.Model, req.Role)
}

// unregisterInstance is the north interface to unregister instance for globalScheduler.
// Notice: this function is optional, only used for testing.
func (s *HttpServer) unregisterInstance(w http.ResponseWriter, r *http.Request) {
	if s.manager.IsEnableZK() {
		err := fmt.Errorf("the zookeeper manager is enable, please use zookeeper to register or unregister")
		http.Error(w, err.Error(), http.StatusForbidden)
		log.Error().Msgf("%v", err)
		return
	}

	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if r.Body == nil || r.ContentLength == 0 {
		err := fmt.Errorf("the body is None")
		http.Error(w, err.Error(), http.StatusBadRequest)
		log.Error().Msgf("failed to decode unregister instance, err: %v", err)
		return
	}

	var req base.UnregisterInstanceIn
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		log.Error().Msgf("failed to decode unregister instance, err: %v", err)
		return
	}

	log.Info().Msgf("start to unregister instance (%v:%v), model %v", req.IP, req.Port, req.Model)

	if err := s.manager.UnregisterInstance(&req); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		log.Error().Msgf("failed to unregister instance, err: %v", err)
		return
	}

	w.WriteHeader(http.StatusOK)
	log.Info().Msgf("unregister instance (%v:%v) successfully, model %v", req.IP, req.Port, req.Model)
}

func processMessages(messages []base.OpenAiMessage) string {
	prompt := ""
	for _, m := range messages {
		prompt += m.Role + ":" + m.Content + " "
	}
	return prompt
}

// extractHeaders extracts HTTP headers relevant to consistent hash routing
// from the request into a map[string]string. Only session-affinity related
// headers are extracted to avoid passing unnecessary data.
func extractHeaders(r *http.Request) map[string]string {
	sessionHeaders := []string{
		"X-Session-Id",
		"X-Agent-Id", // Phase 2: agent identity for KVC management (implicit heartbeat)
		"X-User-Id",
		"X-Tenant-Id",
		"X-Correlation-Id",
		"X-Request-Id",
		"X-Trace-Id",
	}
	headers := make(map[string]string)
	for _, name := range sessionHeaders {
		if v := r.Header.Get(name); v != "" {
			headers[name] = v
		}
	}
	return headers
}

// scheduleForOpenAi is the north data plane interface, it is used for giving schedule
// suggestion based on load of instances.
func (s *HttpServer) scheduleForOpenAi(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if r.Body == nil || r.ContentLength == 0 {
		err := fmt.Errorf("the body is None")
		http.Error(w, err.Error(), http.StatusBadRequest)
		log.Error().Msgf("failed to get suggestion, err: %v", err)
		return
	}

	var req base.OpenAiRequest

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	log.Debug().Msgf("processing schedule request, UUID: %v, model: %v", req.UUID, req.Model)
	prompt := processMessages(req.Messages)
	if prompt == "" {
		log.Error().Msgf("prompt is empty")
		http.Error(w, "prompt is empty", http.StatusBadRequest)
		return
	}

	aigwCfg := serverHandler.cfgMgr.GetAigwConfig()
	if len([]rune(prompt)) > aigwCfg.Limits.MaxPromptRunes {
		log.Error().Msgf("prompt is too long, characters nums: %v", len([]rune(prompt)))
		http.Error(w, "prompt is too long", http.StatusBadRequest)
		return
	}

	if m, _ := s.manager.GetModelMode(req.Model); m == "provider" {
		http.Error(w, "model is in provider mode, must use /chat/completions endpoint", http.StatusBadRequest)
		return
	}

	in := &core.GetSuggestionIn{
		UUID:    req.UUID,
		Model:   req.Model,
		Prompt:  prompt,
		Ctx:     r.Context(),
		Headers: extractHeaders(r),
	}

	out, err := s.manager.GetSuggestion(in)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	// Phase 2: implicit heartbeat — when KVC management is enabled and
	// implicitHeartbeatFromRequests is true, a schedule request carrying X-Agent-Id
	// counts as a heartbeat (agent is alive, doing inference). Lets agents that don't
	// call /agents/{id}/heartbeat still be tracked for fault recovery.
	if aigwCfg.Kvc.Enabled && aigwCfg.Kvc.Agent.ImplicitHeartbeatFromRequests {
		if reg := s.manager.GetAgentRegistry(); reg != nil {
			if agentID := in.Headers["X-Agent-Id"]; agentID != "" {
				var sessionIDs []string
				if sid := in.Headers["X-Session-Id"]; sid != "" {
					sessionIDs = []string{sid}
				}
				_ = reg.Heartbeat(agentID, nil, sessionIDs)
			}
		}
	}

	jsonData, err := json.Marshal(out)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusOK)
	if _, err = w.Write(jsonData); err != nil {
		log.Warn().Msgf("Error writing response: %v", err)
	}
}

// forwardChatCompletions forwards chat completion requests to the target worker.
// This is used when AIGW acts as a proxy/gateway rather than just returning scheduling suggestions.
func (s *HttpServer) forwardChatCompletions(w http.ResponseWriter, r *http.Request) {
	if r.Method != "POST" {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if r.Body == nil || r.ContentLength == 0 {
		http.Error(w, "Request body is empty", http.StatusBadRequest)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, fmt.Sprintf("Failed to read request body: %v", err), http.StatusBadRequest)
		return
	}

	var req struct {
		Model    string `json:"model"`
		Stream   bool   `json:"stream"`
		Messages []struct {
			Role    string `json:"role"`
			Content string `json:"content"`
		} `json:"messages"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		http.Error(w, fmt.Sprintf("Failed to parse request: %v", err), http.StatusBadRequest)
		return
	}

	if req.Model == "" {
		http.Error(w, "Model is required", http.StatusBadRequest)
		return
	}

	mode, err := s.manager.GetModelMode(req.Model)
	if err != nil {
		http.Error(w, "unknown model", http.StatusNotFound)
		return
	}
	switch mode {
	case "provider":
		s.forwardToProvider(w, r, req.Model, req.Stream, body)
	default:
		s.forwardToInstance(w, r, req.Model, req.Stream, body)
	}
}

func (s *HttpServer) forwardToInstance(w http.ResponseWriter, r *http.Request, model string, stream bool, body []byte) {
	var req struct {
		Messages []struct {
			Role    string `json:"role"`
			Content string `json:"content"`
		} `json:"messages"`
	}
	_ = json.Unmarshal(body, &req)

	log.Debug().Msgf("Forwarding chat completion request, model: %s, stream: %v", model, stream)

	prompt := ""
	for _, m := range req.Messages {
		prompt += m.Role + ":" + m.Content + " "
	}

	var bodyMap map[string]interface{}
	json.Unmarshal(body, &bodyMap)

	sessionID := gs.ExtractSessionID(bodyMap)

	in := &core.GetSuggestionIn{
		UUID:      fmt.Sprintf("req-%d", time.Now().UnixNano()),
		Model:     model,
		Prompt:    prompt,
		Ctx:       r.Context(),
		Headers:   extractHeaders(r),
		Body:      bodyMap,
		SessionID: sessionID,
	}

	suggestion, err := s.manager.GetSuggestion(in)
	if err != nil {
		log.Error().Msgf("Failed to get scheduling suggestion: %v", err)
		http.Error(w, fmt.Sprintf("Failed to get scheduling suggestion: %v", err), http.StatusInternalServerError)
		if sessionID != "" {
			// No suggestion -> request was never dispatched -> nothing to free.
			s.manager.ErrOnCancel(model, sessionID, "")
		}
		return
	}

	targetURL := suggestion.TargetPrefillUrl
	if targetURL == "" {
		targetURL = suggestion.TargetDecodeUrl
	}
	if targetURL == "" {
		http.Error(w, "No available worker instance", http.StatusServiceUnavailable)
		if sessionID != "" {
			// suggestion exists but has no instance URL (TargetPrefillUrl == "").
			s.manager.ErrOnCancel(model, sessionID, suggestion.TargetPrefillUrl)
		}
		return
	}
	// Strip the "@rank" suffix: it is an aigw-internal DP-aware instance
	// identifier (see dp_worker.go), not part of the real downstream HTTP URL.
	// The DP rank is conveyed to the downstream via suggestion.DpRank (header).
	targetURL, _, _ = gs.ParseDPAwareWorkerURL(targetURL)
	if !strings.HasPrefix(targetURL, "http://") && !strings.HasPrefix(targetURL, "https://") {
		targetURL = "http://" + targetURL
	}

	log.Debug().Msgf("Forwarding request to: %s, DpRank: %v, Priority: %v", targetURL, suggestion.DpRank, suggestion.Priority)

	forwardBody := body
	if suggestion.Priority != nil {
		forwardBody = injectPriority(body, *suggestion.Priority)
	}

	forwardReq := &proxy.ForwardRequest{
		Method:    "POST",
		TargetURL: targetURL,
		Route:     "/v1/chat/completions",
		Headers:   r.Header,
		Body:      forwardBody,
		Stream:    stream,
		DpRank:    suggestion.DpRank,
	}

	result, err := s.proxyMgr.ForwardRequest(r.Context(), forwardReq)
	if err != nil {
		log.Error().Msgf("Forward request failed: %v", err)
		http.Error(w, fmt.Sprintf("Forward request failed: %v", err), http.StatusBadGateway)
		if sessionID != "" {
			// Request was dispatched (OnDispatched already counted it); free its slot.
			s.manager.ErrOnCancel(model, sessionID, suggestion.TargetPrefillUrl)
		}
		return
	}

	var finishReason string

	if stream && result.StreamReader != nil {
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("Connection", "keep-alive")
		w.WriteHeader(http.StatusOK)

		for {
			event, err := result.StreamReader.ReadEvent()
			if err == io.EOF {
				log.Debug().Msg("SSE stream completed")
				break
			}
			if err != nil {
				log.Error().Msgf("SSE read error: %v", err)
				if finishReason == "" {
					finishReason = "cancelled"
				}
				break
			}
			if fr := parseFinishReason(event.Data); fr != "" {
				finishReason = fr
			}
			if event.Event != "" {
				fmt.Fprintf(w, "event: %s\n", event.Event)
			}
			fmt.Fprintf(w, "data: %s\n\n", event.Data)
			if flusher, ok := w.(http.Flusher); ok {
				flusher.Flush()
			}
		}
	} else {
		for k, v := range result.Headers {
			if k == "Transfer-Encoding" || k == "Connection" {
				continue
			}
			w.Header()[k] = v
		}
		w.WriteHeader(result.StatusCode)
		if _, err := w.Write(result.Body); err != nil {
			log.Warn().Msgf("Error writing response: %v", err)
		}
		finishReason = parseFinishReasonFromJSON(result.Body)
	}

	if sessionID != "" {
		if finishReason == "" {
			finishReason = "unknown"
		}
		// Free the instance slot this request used (== OnDispatched's key, since
		// suggestion.TargetPrefillUrl == result.PrefillUrl). HTTP-level, not session.
		s.manager.ErrOnResponse(model, sessionID, suggestion.TargetPrefillUrl, finishReason)
	}
}

// injectPriority injects the ERR priority value into the request body's
// agent_hint.priority_control.priority field. priority is int (truncated E[rem])
// so the value used for local queue ordering == the value the downstream sees.
// See review [31]/[6]; revert to float if the downstream protocol switches.
func injectPriority(body []byte, priority int) []byte {
	var bodyMap map[string]interface{}
	if err := json.Unmarshal(body, &bodyMap); err != nil {
		return body
	}

	agentHint, ok := bodyMap["agent_hint"].(map[string]interface{})
	if !ok {
		agentHint = make(map[string]interface{})
	}

	priorityControl, ok := agentHint["priority_control"].(map[string]interface{})
	if !ok {
		priorityControl = make(map[string]interface{})
	}

	priorityControl["priority"] = priority
	agentHint["priority_control"] = priorityControl
	bodyMap["agent_hint"] = agentHint

	modified, err := json.Marshal(bodyMap)
	if err != nil {
		return body
	}
	return modified
}

// parseFinishReason parses finish_reason from an SSE data line.
func parseFinishReason(data string) string {
	if data == "" || data == "[DONE]" {
		return ""
	}
	var chunk struct {
		Choices []struct {
			FinishReason string `json:"finish_reason"`
		} `json:"choices"`
	}
	if err := json.Unmarshal([]byte(data), &chunk); err != nil {
		return ""
	}
	if len(chunk.Choices) > 0 && chunk.Choices[0].FinishReason != "" {
		return chunk.Choices[0].FinishReason
	}
	return ""
}

// parseFinishReasonFromJSON parses finish_reason from a non-streaming JSON response body.
func parseFinishReasonFromJSON(body []byte) string {
	if len(body) == 0 {
		return ""
	}
	var resp struct {
		Choices []struct {
			FinishReason string `json:"finish_reason"`
		} `json:"choices"`
	}
	if err := json.Unmarshal(body, &resp); err != nil {
		return ""
	}
	if len(resp.Choices) > 0 {
		return resp.Choices[0].FinishReason
	}
	return ""
}

func (s *HttpServer) forwardToProvider(w http.ResponseWriter, r *http.Request, model string, stream bool, body []byte) {
	pool := s.manager.GetApiPool(model)
	if pool == nil {
		http.Error(w, "unknown model", http.StatusNotFound)
		return
	}
	pctx := &apipool.Context{Model: model, Stream: stream}
	triedKeys := map[apipool.StateKey]bool{}
	var lastStatus int

	maxFailover := pool.MaxFailoverEndpoints()
	log.Debug().Msgf("forwardToProvider: model=%s stream=%v maxFailover=%d", model, stream, maxFailover)

	for attempt := 0; attempt < maxFailover; attempt++ {
		if r.Context().Err() != nil {
			return
		}
		dep := pool.SelectExcept(pctx, triedKeys)
		if dep == nil {
			log.Debug().Msgf("forwardToProvider: model=%s attempt=%d no deployment available, stopping", model, attempt)
			break
		}
		triedKeys[dep.StateKey()] = true

		adapter, err := pool.GetAdapter(dep.Provider)
		if err != nil {
			log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s provider=%s no adapter: %v", model, attempt, dep.ID, dep.Provider, err)
			continue
		}
		headers := r.Header.Clone()
		adapter.InjectAuth(headers, dep)

		reqBody := body
		if dep.ModelName != "" && dep.ModelName != model {
			if rewritten, rerr := rewriteModelField(body, dep.ModelName); rerr == nil {
				reqBody = rewritten
				log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s rewrote upstream model -> %s", model, attempt, dep.ID, dep.ModelName)
			} else {
				log.Warn().Msgf("forwardToProvider: model=%s attempt=%d id=%s model rewrite failed, forwarding original: %v", model, attempt, dep.ID, rerr)
			}
		}

		fullURL := adapter.BuildURL(dep, "/v1/chat/completions", stream)
		forwardReq := &proxy.ForwardRequest{
			Method:  "POST",
			FullURL: fullURL,
			Headers: headers,
			Body:    reqBody,
			Stream:  stream,
		}
		log.Debug().Msgf("forwardToProvider: model=%s attempt=%d forwarding to id=%s provider=%s url=%s", model, attempt, dep.ID, dep.Provider, fullURL)

		start := time.Now()
	// The aigw→LLM forward is bounded by the upstream's HTTP context
	// (r.Context(): the client gives up / closes the connection on its own
	// timeout) plus the proxy's http.Client.Timeout (proxy.timeout, for a hung
	// downstream when the client is patient). No per-model singleRoundTimeout:
	// it was redundant with these two and only wired into this provider path
	// anyway. See review [15]/④.
	fwdCtx := r.Context()

	result, err := s.proxyMgr.ForwardRequest(fwdCtx, forwardReq)
		if err != nil {
			kind := pool.OnFailure(dep, 0, err)
			log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s transport error kind=%v latencyMs=%d: %v", model, attempt, dep.ID, kind, time.Since(start).Milliseconds(), err)
			if kind == apipool.ErrCanceled {
				return
			}
			continue
		}
		if result.StatusCode >= 400 {
			lastStatus = result.StatusCode
			kind := pool.OnFailure(dep, result.StatusCode, nil)
			log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s status=%d kind=%v latencyMs=%d", model, attempt, dep.ID, result.StatusCode, kind, time.Since(start).Milliseconds())
			if kind == apipool.ErrClient4xx {
				if result.StreamReader != nil {
					_ = result.StreamReader.Close()
				}
				writeProviderNonStream(w, result)
				return
			}
			if result.StreamReader != nil {
				_ = result.StreamReader.Close()
			}
			continue
		}

		if stream && result.StreamReader != nil {
			log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s streaming response started", model, attempt, dep.ID)
			s.streamProviderResponse(w, pool, dep, result, start)
			return
		}
		tokens := parseUsageTokens(result.Body)
		latency := time.Since(start)
		pool.OnSuccess(dep, latency, tokens)
		log.Debug().Msgf("forwardToProvider: model=%s attempt=%d id=%s success status=%d tokens=%d latencyMs=%d", model, attempt, dep.ID, result.StatusCode, tokens, latency.Milliseconds())
		writeProviderNonStream(w, result)
		return
	}

	if lastStatus != 0 {
		log.Debug().Msgf("forwardToProvider: model=%s all providers failed, last status=%d", model, lastStatus)
		http.Error(w, "all providers failed", lastStatus)
		return
	}
	log.Debug().Msgf("forwardToProvider: model=%s all providers failed, no upstream status", model)
	http.Error(w, "all providers failed", http.StatusBadGateway)
}

func (s *HttpServer) streamProviderResponse(w http.ResponseWriter, pool *apipool.ApiPoolManager, dep *apipool.Deployment, result *proxy.ForwardResult, start time.Time) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(http.StatusOK)

	defer func() { _ = result.StreamReader.Close() }()
	firstChunk := false
	tokens := 0
	for {
		event, err := result.StreamReader.ReadEvent()
		if err == io.EOF {
			pool.OnStreamSuccess(dep, tokens)
			log.Debug().Msgf("streamProviderResponse: id=%s stream complete tokens=%d durationMs=%d", dep.ID, tokens, time.Since(start).Milliseconds())
			return
		}
		if err != nil {
			if !firstChunk {
				pool.OnFailure(dep, 0, err)
				log.Debug().Msgf("streamProviderResponse: id=%s stream error before first chunk: %v", dep.ID, err)
			}
			return
		}
		if !firstChunk {
			firstChunk = true
			pool.OnFirstChunk(dep, time.Since(start))
			log.Debug().Msgf("streamProviderResponse: id=%s first chunk ttftMs=%d", dep.ID, time.Since(start).Milliseconds())
		}
		if event.Event != "" {
			fmt.Fprintf(w, "event: %s\n", event.Event)
		}
		fmt.Fprintf(w, "data: %s\n\n", event.Data)
		if flusher, ok := w.(http.Flusher); ok {
			flusher.Flush()
		}
		if t := parseUsageTokens([]byte(event.Data)); t > 0 {
			tokens = t
		}
	}
}

func writeProviderNonStream(w http.ResponseWriter, result *proxy.ForwardResult) {
	for k, v := range result.Headers {
		if k == "Transfer-Encoding" || k == "Connection" {
			continue
		}
		w.Header()[k] = v
	}
	w.WriteHeader(result.StatusCode)
	_, _ = w.Write(result.Body)
}

// rewriteModelField replaces the top-level "model" field in an OpenAI-style JSON
// request body, preserving all other fields byte-for-byte.
func rewriteModelField(body []byte, model string) ([]byte, error) {
	var m map[string]json.RawMessage
	if err := json.Unmarshal(body, &m); err != nil {
		return nil, err
	}
	mv, err := json.Marshal(model)
	if err != nil {
		return nil, err
	}
	m["model"] = mv
	return json.Marshal(m)
}

// parseUsageTokens extracts usage.total_tokens from an OpenAI-style JSON body; 0 if absent.
func parseUsageTokens(body []byte) int {
	var parsed struct {
		Usage struct {
			TotalTokens int `json:"total_tokens"`
		} `json:"usage"`
	}
	if err := json.Unmarshal(body, &parsed); err != nil {
		return 0
	}
	return parsed.Usage.TotalTokens
}

func (s *HttpServer) checkHealth(errChan <-chan error) error {
	// Wait for the server to be ready
	ticker := time.NewTicker(waitServerReadyDelay)
	defer ticker.Stop()
	healthUrl := fmt.Sprintf("http://%s/aigw/v1/health", s.server.Addr)
	client := &http.Client{Timeout: waitServerReadyTimeout}
	startTime := time.Now().UTC()
	req, err := http.NewRequest("GET", healthUrl, nil)
	if err != nil {
		log.Error().Msgf("HTTP server failed to start, err: %v", err)
		return err
	}
	if s.serHmacMgr.EnableHmac() {
		err = s.serHmacMgr.AddHmacSign(req, "")
		if err != nil {
			log.Error().Msgf("HTTP server failed to start, err: %v", err)
			return err
		}

	}
	for {
		select {
		case <-ticker.C:
			resp, err := client.Do(req)
			if err != nil {
				log.Error().Msgf("health check error: %v", err.Error())
				return err
			}
			defer resp.Body.Close()

			if err == nil && resp.StatusCode == http.StatusOK {
				log.Info().Msgf("HTTP server is successfully started")
				s.setReady()
				return nil
			}
			// add a timeout to avoid infinite waiting
			if time.Since(startTime) > waitServerReadyTimeout {
				e1 := fmt.Errorf("server did not start within the expected time %v", waitServerReadyTimeout)
				log.Error().Msgf("%v", e1)
				return e1
			}

		case err := <-errChan:
			log.Error().Msgf("HTTP server failed to start, err: %v", err)
			return err
		}
	}
}
