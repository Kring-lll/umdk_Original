/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: schedule implementation.
 * Create: 2025-05-13
 */

// Package gs is the global scheduler for aigw.
package gs

import (
	"context"
	"sort"
	"strings"

	"huawei.com/aigw/internal/kvevents"
	"huawei.com/aigw/internal/prefixcache"
	"huawei.com/aigw/internal/renderclient"
	"huawei.com/aigw/pkg/log"
)

type prefixCacheLoadBalancer struct {
	baseLoadBalancer

	modelName      string
	prefixCacheMgr prefixcache.PrefixCacheManager
	renderClient   *renderclient.RenderClient
	// fallbackLB runs when prefix matching yields no usable target.
	// Token-based (TokenNum), not leastConn (ReqNum). #899.
	fallbackLB loadBalancer
	// loadImbalanceThreshold: when max(TokenNum)-min(TokenNum) across ready
	// pods exceeds it, prioritize load balancing over prefix hit. 0 disables.
	loadImbalanceThreshold int
}

func newPrefixCacheLB(metricProvider MetricProvider, params *AlgorithmParams) (*prefixCacheLoadBalancer, error) {
	log.Info().Msg("[prefixCache] Init prefixCache loadbalancer.")

	pcConfig := prefixcache.DefaultConfig()
	pcConfig.Enabled = true
	// Align block size with the per-model blockSize (vLLM --block-size). env still wins.
	if params.BlockSize > 0 {
		pcConfig.BlockSize = params.BlockSize
	}

	kveventsConfig := kvevents.DefaultKVEventsManagerConfig()
	var extraHandlers []kvevents.EventHandler
	if params.KvcSessionMgr != nil {
		// Phase 2: fan out kvevents to both prefixcache and the per-model KvcSessionManager.
		extraHandlers = append(extraHandlers, params.KvcSessionMgr.AsKveventsHandler())
	}
	pcMgr, err := prefixcache.NewPrefixCacheManager(pcConfig, kveventsConfig, extraHandlers...)
	if err != nil {
		log.Warn().Msgf("[prefixCache] failed to create prefix cache manager: %v", err)
		return nil, err
	}

	// See fallbackLB above. #899.
	fallback, err := newTokenLB(metricProvider, params)
	if err != nil {
		log.Warn().Msgf("[prefixCache] failed to create fallback token LB: %v", err)
	}

	return &prefixCacheLoadBalancer{
		baseLoadBalancer: baseLoadBalancer{
			metricProvider:   metricProvider,
			instanceRoleType: params.InstanceRoleType,
		},
		prefixCacheMgr:         pcMgr,
		loadImbalanceThreshold: params.LoadImbalanceThreshold,
		fallbackLB:             fallback,
	}, nil
}

func (lb *prefixCacheLoadBalancer) schedule(request *ScheduleRequestMsg, options *ScheduleOptions) *ScheduleResult {
	// Write back prompt tokens so TokenNum grows (prefixCache counts prompt
	// tokens only); otherwise the load-imbalance branch never fires.
	if request.Request != nil && request.Request.PromptLen > 0 {
		request.Request.PredictTokens = request.Request.PromptLen
	}

	readyInstances, err := lb.getReadyInstances(request.CandidateInstanceIDs, options)
	if err != nil {
		log.Error().Msgf("[prefixCache] failed to get ready instances: %v", err)
		return lb.fallback(request, options)
	}

	if len(readyInstances) == 0 {
		log.Debug().Msg("[prefixCache] no ready instances")
		return lb.fallback(request, options)
	}

	readyInstanceNames := make([]string, 0, len(readyInstances))
	urlToInstanceName := make(map[string]string, len(readyInstances))
	for _, url := range readyInstances {
		if instanceName, ok := lb.prefixCacheMgr.GetInstanceNameByUrl(url); ok {
			readyInstanceNames = append(readyInstanceNames, instanceName)
			urlToInstanceName[url] = instanceName
		} else {
			log.Debug().Msgf("[prefixCache] no instance name mapping for URL %s", url)
		}
	}

	if len(readyInstanceNames) == 0 {
		log.Debug().Msg("[prefixCache] no ready instances after URL->name mapping, using original URLs")
		readyInstanceNames = readyInstances
		// Fall back: map URL to itself.
		urlToInstanceName = make(map[string]string, len(readyInstances))
		for _, url := range readyInstances {
			urlToInstanceName[url] = url
		}
	}

	modelName := lb.modelName
	if modelName == "" {
		modelName = "default"
	}

	var tokenIDs []int64 // chat-template domain (render)
	var usePrefixCache bool

	if lb.renderClient != nil && request.Request != nil {
		log.Debug().Msg("[prefixCache] branch: using render client for tokenization")
		messages := lb.buildChatMessages(request.Request)
		// Forward the inbound Authorization header to the render endpoint.
		authHeader := request.Headers["Authorization"]
		tokenIDs, err = lb.prefixCacheMgr.Tokenize(context.Background(), modelName, messages, authHeader)
		if err != nil || len(tokenIDs) == 0 {
			log.Error().Msgf("[prefixCache] branch: render tokenization failed: %v, tokenIDs=%v", err, tokenIDs)
		} else {
			usePrefixCache = true
		}
	}

	var matched map[string]int
	var matchLength int

	if usePrefixCache {
		// Match on the render (chat-template) domain only. /tokenize per
		// request and dual-domain merge are bypassed (TokenizePrompt/
		// mergeMatched remain as API for when a worker's reported domain is known).
		matched, matchLength, err = lb.prefixCacheMgr.MatchPrefix(
			modelName,
			-1,
			tokenIDs,
			readyInstanceNames,
		)
		if err != nil {
			log.Error().Msgf("[prefixCache] branch: template-domain prefix match failed: %v", err)
		}
	} else if request.Request != nil {
		log.Debug().Msgf("[prefixCache] branch: using text-based fallback prefix match, readyInstanceNames=%v", readyInstanceNames)
		matched, matchLength, err = lb.prefixCacheMgr.MatchPrefixByText(
			modelName,
			-1,
			request.Request.Prompt,
			readyInstanceNames,
		)
	}

	if len(matched) > 0 {
		targetInstance := lb.selectFromMatched(readyInstances, matched, urlToInstanceName, options)
		if targetInstance != nil {
			log.Debug().Msgf("[prefixCache] branch: selected target instance %s", targetInstance.InsUrl)

			// Report the selected pod's own match percent (imbalance branch may
			// pick a zero-match pod); fall back to global matchLength.
			selMatchPct := 0
			if selName, ok := urlToInstanceName[targetInstance.InsUrl]; ok {
				if pct, hasMatch := matched[selName]; hasMatch {
					selMatchPct = pct
				}
			}
			log.Debug().Msgf("[prefixCache] req %v scheduled to %s with match %d%% (global max matchLen %d)",
				request.Request.ReqId, targetInstance.InsUrl, selMatchPct, matchLength)

			return &ScheduleResult{
				ResultType:     DispatchRequest,
				PrefillUrl:     targetInstance.InsUrl,
				DecodeUrl:      "",
				TokenIds:       "",
				PrefillGroupID: targetInstance.GroupID,
				DecodeGroupID:  "",
			}
		} else {
			log.Debug().Msg("[prefixCache] branch: no valid target instance from matched")
		}
	} else {
		log.Debug().Msg("[prefixCache] branch: no prefix match found")
	}

	log.Debug().Msg("[prefixCache] branch: falling back to LB")
	return lb.fallback(request, options)
}

func (lb *prefixCacheLoadBalancer) fallback(request *ScheduleRequestMsg, options *ScheduleOptions) *ScheduleResult {
	if lb.fallbackLB == nil {
		log.Debug().Msg("[prefixCache] fallback LB not available")
		return createEmptyScheduleResult()
	}

	return lb.fallbackLB.schedule(request, options)
}

// selectFromMatched picks a target pod: if token-load spread across ready pods
// exceeds loadImbalanceThreshold, return the lowest-load pod; otherwise the
// highest prefix-match pod (ties: lower load). Threshold 0 = prefix-only.
func (lb *prefixCacheLoadBalancer) selectFromMatched(instances []string, matched map[string]int, urlToInstanceName map[string]string, options *ScheduleOptions) *InstanceMetric {
	type instanceScore struct {
		metric       *InstanceMetric
		instanceName string
		matchPercent int
	}

	metrics, err := lb.metricProvider.GetInstanceMetrics(instances, lb.buildQueryOptions(options))
	if err != nil || len(metrics) == 0 {
		log.Warn().Msgf("[prefixCache] selectFromMatched: no metrics found for instances: %v", instances)
		return nil
	}

	// matched is keyed by instanceName; metrics by URL — bridge via urlToInstanceName.
	scores := make([]instanceScore, 0, len(metrics))
	for _, m := range metrics {
		instanceName, ok := urlToInstanceName[m.InsUrl]
		if !ok {
			log.Debug().Msgf("[prefixCache] selectFromMatched: no instance name mapping for url %s", m.InsUrl)
			continue
		}

		matchPercent, hasMatch := matched[instanceName]
		if hasMatch {
			log.Debug().Msgf("[prefixCache] selectFromMatched: matched instance %s (url=%s) with %d%%, load=%d",
				instanceName, m.InsUrl, matchPercent, m.TokenNum)
		} else {
			log.Debug().Msgf("[prefixCache] selectFromMatched: instance %s (url=%s) no prefix match, load=%d",
				instanceName, m.InsUrl, m.TokenNum)
			matchPercent = 0
		}

		scores = append(scores, instanceScore{
			metric:       m,
			instanceName: instanceName,
			matchPercent: matchPercent,
		})
	}

	if len(scores) == 0 {
		log.Debug().Msgf("[prefixCache] selectFromMatched: no ready instances with metrics")
		return nil
	}

	// Branch 1: load imbalance — spread > threshold => lowest-load pod.
	if lb.loadImbalanceThreshold > 0 {
		minIdx := 0
		maxIdx := 0
		for i := 1; i < len(scores); i++ {
			if scores[i].metric.TokenNum < scores[minIdx].metric.TokenNum ||
				(scores[i].metric.TokenNum == scores[minIdx].metric.TokenNum &&
					scores[i].instanceName < scores[minIdx].instanceName) {
				minIdx = i
			}
			if scores[i].metric.TokenNum > scores[maxIdx].metric.TokenNum {
				maxIdx = i
			}
		}
		spread := scores[maxIdx].metric.TokenNum - scores[minIdx].metric.TokenNum
		if spread > lb.loadImbalanceThreshold {
			log.Debug().Msgf("[prefixCache] selectFromMatched: imbalanced (spread=%d > %d), pick lowest %s (load=%d, match=%d%%)",
				spread, lb.loadImbalanceThreshold, scores[minIdx].instanceName,
				scores[minIdx].metric.TokenNum, scores[minIdx].matchPercent)
			return scores[minIdx].metric
		}
		log.Debug().Msgf("[prefixCache] selectFromMatched: balanced (spread=%d <= %d), pick by match",
			spread, lb.loadImbalanceThreshold)
	}

	// Branch 2: balanced — highest match, then lowest load, then name.
	sort.SliceStable(scores, func(i, j int) bool {
		if scores[i].matchPercent != scores[j].matchPercent {
			return scores[i].matchPercent > scores[j].matchPercent
		}
		if scores[i].metric.TokenNum != scores[j].metric.TokenNum {
			return scores[i].metric.TokenNum < scores[j].metric.TokenNum
		}
		return scores[i].instanceName < scores[j].instanceName
	})

	log.Debug().Msgf("[prefixCache] selectFromMatched: selected %s with %d%% match (load=%d)",
		scores[0].instanceName, scores[0].matchPercent, scores[0].metric.TokenNum)
	return scores[0].metric
}

func (lb *prefixCacheLoadBalancer) getReadyInstances(candidateIDs []string, options *ScheduleOptions) ([]string, error) {
	metrics, err := lb.metricProvider.GetInstanceMetrics(candidateIDs, lb.buildQueryOptions(options))
	if err != nil {
		return nil, err
	}

	result := make([]string, 0, len(metrics))
	for _, m := range metrics {
		result = append(result, m.InsUrl)
	}

	return result, nil
}

func (lb *prefixCacheLoadBalancer) buildChatMessages(request *LlmRequest) []prefixcache.ChatMessage {
	if request == nil {
		return nil
	}

	messages := make([]prefixcache.ChatMessage, 0, 1)

	if request.Prompt != "" {
		// Reverse processMessages format: "role:content " -> role, content.
		prompt := strings.TrimSpace(request.Prompt)
		idx := strings.Index(prompt, ":")
		if idx > 0 {
			role := prompt[:idx]
			content := prompt[idx+1:]
			messages = append(messages, prefixcache.ChatMessage{
				Role:    role,
				Content: content,
			})
		} else {
			messages = append(messages, prefixcache.ChatMessage{
				Role:    "user",
				Content: prompt,
			})
		}
	}

	return messages
}

// buildContentText joins message contents in the raw content domain (no chat
// template). Retained for content-domain matching; currently bypassed.
func (lb *prefixCacheLoadBalancer) buildContentText(messages []prefixcache.ChatMessage) string {
	if len(messages) == 0 {
		return ""
	}

	parts := make([]string, 0, len(messages))
	for _, msg := range messages {
		if strings.TrimSpace(msg.Content) == "" {
			continue
		}
		parts = append(parts, msg.Content)
	}
	return strings.Join(parts, "\n")
}

// mergeMatched merges both token domains, keeping the higher match percent and
// its domain's tokenIDs. Retained for dual-domain matching; currently bypassed.
func mergeMatched(
	templateMatched map[string]int, templateIDs []int64,
	contentMatched map[string]int, contentIDs []int64,
) (map[string]int, map[string][]int64) {
	merged := make(map[string]int, len(templateMatched)+len(contentMatched))
	domainTokens := make(map[string][]int64)

	for inst, pct := range templateMatched {
		merged[inst] = pct
		domainTokens[inst] = templateIDs
	}
	for inst, pct := range contentMatched {
		if cur, ok := merged[inst]; !ok || pct > cur {
			merged[inst] = pct
			domainTokens[inst] = contentIDs
		}
	}

	return merged, domainTokens
}

func (lb *prefixCacheLoadBalancer) SetRenderClient(client *renderclient.RenderClient) {
	lb.renderClient = client
	if lb.prefixCacheMgr != nil {
		lb.prefixCacheMgr.SetRenderClient(client)
	}
}

func (lb *prefixCacheLoadBalancer) SetModelName(modelName string) {
	lb.modelName = modelName
}

// PrefixCacheCapable exposes the prefix cache manager without coupling callers
// to prefixCacheLoadBalancer.
type PrefixCacheCapable interface {
	GetPrefixCacheManager() prefixcache.PrefixCacheManager
}

func (lb *prefixCacheLoadBalancer) GetPrefixCacheManager() prefixcache.PrefixCacheManager {
	return lb.prefixCacheMgr
}

func (lb *prefixCacheLoadBalancer) Stop() {
	if lb.prefixCacheMgr != nil {
		lb.prefixCacheMgr.Stop()
	}
}
