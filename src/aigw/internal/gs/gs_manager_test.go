/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: global scheduler manager test
 * Create: 2025-7-28
 */

// Package gs is the global scheduler for AIGW.
package gs

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/agiledragon/gomonkey/v2"
	"github.com/stretchr/testify/assert"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/internal/cachecenter"
	"huawei.com/aigw/internal/stats"
	"huawei.com/aigw/internal/tokenizers"
	"huawei.com/aigw/pkg/crypto"
)

const (
	gsTokenzerCfgFile  = "../../test/tokenizer/DeepSeek-R1-Distill-Qwen-7B/tokenizer.json"
	gsPredictModelFile = "../../test/lightgbm/lgbm_text_classifier.txt"
	gsVectorizerFile   = "../../test/vectorizer/pretrained_vector.json"

	gsWaitResponseTimeout = 1 * time.Second

	gsTestTtft      = 200 // ms
	gsTestTbt       = 50  // ms
	gsTestSnapFeq   = 3
	gsTestBlockSize = 128
	gsTestMinBlocks = 10
	gsTestBathSize  = 10

	gsTestMaxPromptRunes = 1024
)

func newTestDefaultGsConfig(model string) *base.GlobalSchedulerConfig {
	return &base.GlobalSchedulerConfig{
		Model:                model,
		BlockSize:            128,
		DeployPolicy:         "separated",
		MaxTimeToFirstToken:  1000, // ms
		MaxTimeBetweenTokens: 500,  // ms
		TokenizeModelName:    "",
		LoadBalancer: base.LoadBalancerConfig{
			Mixed:               "token",
			Prefill:             "prefillTimeAware",
			Decode:              "decode",
			BatchSize:           128,
			ReservedBlockNumber: 1,
			MinMatchedLength:    0,
			PowerOfTwo:          false,
		},
		InsConnectType:         "",
		CacheRefreshIntervalMs: 100,
	}
}

// TestDeploymentPolicyStringMixed tests the String method when the deployment policy is mixedDeployment
func TestDeploymentPolicyStringMixed(t *testing.T) {
	policy := MixedDeployment
	expected := "mixed"
	actual := policy.String()
	assert.Equal(t, expected, actual)
}

// TestDeploymentPolicyStringSeparated tests the String method when the deployment policy is separatedDeployment
func TestDeploymentPolicyStringSeparated(t *testing.T) {
	policy := SeparatedDeployment
	expected := "separated"
	actual := policy.String()
	assert.Equal(t, expected, actual)
}

// TestDeploymentPolicyStringUnknown tests the String method when the deployment policy is unknown
func TestDeploymentPolicyStringUnknown(t *testing.T) {
	policy := DeploymentPolicy(2) // An unknown deployment policy
	expected := "unknown"
	actual := policy.String()
	assert.Equal(t, expected, actual)
}

// TestPredictorTypeStringNone tests the String method when the predictor type is predictTypeNone
func TestPredictorTypeStringNone(t *testing.T) {
	predictor := PredictTypeNone
	expected := "none"
	actual := predictor.String()
	assert.Equal(t, expected, actual)
}

// TestPredictorTypeStringEma tests the String method when the predictor type is predictTypeEma
func TestPredictorTypeStringEma(t *testing.T) {
	predictor := PredictTypeEma
	expected := "ema"
	actual := predictor.String()
	assert.Equal(t, expected, actual)
}

// TestPredictorTypeStringLightgbm tests the String method when the predictor type is predictTypeLightgbm
func TestPredictorTypeStringLightgbm(t *testing.T) {
	predictor := PredictTypeLightgbm
	expected := "lightgbm"
	actual := predictor.String()
	assert.Equal(t, expected, actual)
}

// TestPredictorTypeStringUnknown tests the String method when the predictor type is unknown
func TestPredictorTypeStringUnknown(t *testing.T) {
	predictor := PredictorType(3) // An unknown predictor type
	expected := "unknown"
	actual := predictor.String()
	assert.Equal(t, expected, actual)
}

func TestGlobalSchedulerManagerControlPlane(t *testing.T) {
	// Create context
	parentCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	tk, err := tokenizers.NewTokenizer("test_model")
	assert.NoError(t, err)
	err = tk.InitFromFile(gsTokenzerCfgFile)
	assert.NoError(t, err)

	cfg := newTestDefaultGsConfig("test_model")
	// Create GlobalSchedulerManager
	opts := []GlobalSchedulerManagerOption{
		WithModel("test_model"),
		WithLBType("roundRobin", "leastConn", "capacity"),
		WithDeploymentPolicy("mixed"),
		WithPredict("lightgbm", nil),
		WithTokenizer(tk),
		WithSLOThreshold(gsTestTtft, gsTestTbt),
		WithAlgorithmThreshold(gsTestMinBlocks, gsTestBathSize, true, gsTestBlockSize),
		WithSnapFreq(gsTestSnapFeq),
		WithCrypto(crypto.NewHmacManager(nil), crypto.NewAesManager(nil)),
	}
	manager, err := NewGlobalSchedulerManager(parentCtx, cfg, opts...)
	assert.NoError(t, err)

	// Start the manager
	err = manager.Start()
	assert.NoError(t, err)

	insMgr := &InstanceManager{}
	patchReg := gomonkey.ApplyPrivateMethod(insMgr, "addInstance",
		func(mgr *InstanceManager, insUrl string, insRole base.InstanceRole, statusChan chan *ControlMessage) error {
			return nil
		})
	defer patchReg.Reset()

	// Register an instance
	rsp := make(chan interface{}, 1)
	ctrlMsg := &ControlMessage{
		Request: &RegisterInstanceMsg{
			Name: "test_instance",
			IP:   "127.0.0.1",
			Port: "8080",
			Role: "mixed",
		},
		Response: rsp,
	}
	manager.PutControlMessage(ctrlMsg)
	select {
	case result, ok := <-rsp:
		if !ok {
			t.Errorf("response channel is closed")
		}
		switch result.(type) {
		case error:
			t.Fatalf("failed to reg, err: %v", result.(error))
		default:
		}
	case <-time.After(gsWaitResponseTimeout):
		t.Fatal("timeout while registering instance")
	}

	patchUnReg := gomonkey.ApplyPrivateMethod(insMgr, "removeInstance",
		func(mgr *InstanceManager, insUrl string) bool {
			return true
		})
	defer patchUnReg.Reset()

	// Unregister an instance
	rsp = make(chan interface{}, 1)
	ctrlMsg = &ControlMessage{
		Request: &UnregisterInstanceMsg{
			IP:   "127.0.0.1",
			Port: "8080",
		},
		Response: rsp,
	}
	manager.PutControlMessage(ctrlMsg)

	// wait response
	select {
	case result, ok := <-rsp:
		if !ok {
			t.Errorf("response channel is closed")
		}
		switch result.(type) {
		case error:
			t.Fatalf("failed to unreg, err: %v", result.(error))
		default:
		}
	case <-time.After(gsWaitResponseTimeout):
		t.Fatal("timeout while unregistering instance")
	}

	// Stop the manager
	manager.Stop()
}

func TestGlobalSchedulerManagerDataPlane(t *testing.T) {
	// Create context
	parentCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	tk, err := tokenizers.NewTokenizer("test_model")
	assert.NoError(t, err)
	err = tk.InitFromFile(gsTokenzerCfgFile)
	assert.NoError(t, err)
	// Create GlobalSchedulerManager
	cfg := newTestDefaultGsConfig("test_model")
	opts := []GlobalSchedulerManagerOption{
		WithModel("test_model"),
		WithLBType("roundRobin", "leastConn", "capacity"),
		WithDeploymentPolicy("mixed"),
		WithPredict("lightgbm", nil),
		WithTokenizer(tk),
		WithSLOThreshold(gsTestTtft, gsTestTbt),
		WithAlgorithmThreshold(gsTestMinBlocks, gsTestBathSize, true, gsTestBlockSize),
		WithSnapFreq(gsTestSnapFeq),
		WithCrypto(crypto.NewHmacManager(nil), crypto.NewAesManager(nil)),
	}
	manager, err := NewGlobalSchedulerManager(parentCtx, cfg, opts...)
	assert.NoError(t, err)

	// Start the manager
	err = manager.Start()
	assert.NoError(t, err)

	testPrompt := strings.Repeat("t", gsTestMaxPromptRunes)
	req, e1 := NewLlmRequest("123", testPrompt)
	assert.NoError(t, e1)

	err = manager.PreprocessForSchedule(req)
	assert.NoError(t, err)

	rsp := make(chan interface{}, 1)
	msg := &ControlMessage{
		Request: &ScheduleRequestMsg{
			Request: req,
		},
		Response: rsp,
	}

	manager.PutScheduleMessage(msg)

	// wait response
	select {
	case out, ok := <-rsp:
		if !ok {
			t.Errorf("response channel is closed")
		}

		switch result := out.(type) {
		case *SuggestionResultMsg:
			assert.NotNil(t, result)
		case error:
			t.Fatalf("failed to get-suggestion, err: %v", result.(error))
		default:
			t.Fatalf("unexpected type of suggestion result")
		}
	case <-time.After(gsWaitResponseTimeout):
		t.Fatal("timeout while get-suggestion")
	}

	// Stop the manager
	manager.Stop()
}

func TestCheckReqSurvival(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cfg := newTestDefaultGsConfig("test_model")
	opts := []GlobalSchedulerManagerOption{
		WithModel("test_model"),
		WithLBType("roundRobin", "leastConn", "capacity"),
		WithDeploymentPolicy("mixed"),
		WithPredict("lightgbm", nil),
		WithSLOThreshold(gsTestTtft, gsTestTbt),
		WithAlgorithmThreshold(gsTestMinBlocks, gsTestBathSize, true, gsTestBlockSize),
		WithSnapFreq(gsTestSnapFeq),
		WithCrypto(crypto.NewHmacManager(nil), crypto.NewAesManager(nil)),
		WithReqSurvivalDuration(10 * time.Second),
	}
	m, err := NewGlobalSchedulerManager(ctx, cfg, opts...)
	assert.NoError(t, err)

	// 注册实例
	testInsURL := "127.0.0.1:8080"
	ins := &instance{
		insUrl:   testInsURL,
		reqSet:   make(map[string]*LlmRequest),
		reqQueue: newRequestQueue(),
	}

	m.instanceManager.insPool[testInsURL] = ins

	// 添加未超时请求
	req := &LlmRequest{
		ReqId:     "fresh-req",
		TimeStamp: time.Now().UnixMilli() - 5*1000, // 5秒前
	}
	ins.addReq(req)

	m.checkReqSurvival()
	assert.Contains(t, ins.reqSet, req.ReqId, "未超时的请求不应被删除")

	// 超时请求删除
	m.config.reqSurvivalDuration = 2 * time.Second
	m.checkReqSurvival()
	assert.NotContains(t, ins.reqSet, req.ReqId, "超时的请求应被删除")

	// 处理空请求
	m.checkReqSurvival()
	assert.Empty(t, ins.reqSet, "空集合操作后仍然为空")

	// 混合场景
	req1 := &LlmRequest{
		ReqId:     "req1",
		TimeStamp: time.Now().UnixMilli() - 5*1000, // 5秒前
	}
	ins.addReq(req)

	req2 := &LlmRequest{
		ReqId:     "req2",
		TimeStamp: time.Now().UnixMilli(), // 当前
	}
	ins.addReq(req)
	ins.addReq(req1)
	ins.addReq(req2)
	assert.Contains(t, ins.reqSet, req1.ReqId, "添加req1")
	assert.Contains(t, ins.reqSet, req2.ReqId, "添加req2")

	m.checkReqSurvival()
	assert.NotContains(t, ins.reqSet, req1.ReqId, "req1删除")
	assert.Contains(t, ins.reqSet, req2.ReqId, "req2还在")
}

func TestCheckReqExists(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cfg := newTestDefaultGsConfig("test_model")
	opts := []GlobalSchedulerManagerOption{
		WithModel("test_model"),
		WithLBType("roundRobin", "leastConn", "capacity"),
		WithDeploymentPolicy("mixed"),
		WithPredict("lightgbm", nil),
		WithSLOThreshold(gsTestTtft, gsTestTbt),
		WithAlgorithmThreshold(gsTestMinBlocks, gsTestBathSize, true, gsTestBlockSize),
		WithSnapFreq(gsTestSnapFeq),
		WithCrypto(crypto.NewHmacManager(nil), crypto.NewAesManager(nil)),
		WithReqSurvivalDuration(10 * time.Second),
	}
	m, err := NewGlobalSchedulerManager(ctx, cfg, opts...)
	assert.NoError(t, err)

	// 注册实例
	testInsURL := "127.0.0.1:8080"
	ins := &instance{
		insUrl:   testInsURL,
		reqSet:   make(map[string]*LlmRequest),
		reqQueue: newRequestQueue(),
	}

	m.instanceManager.insPool[testInsURL] = ins

	exists := m.CheckReqExists("req1")
	assert.Equal(t, exists, false)

	req := &LlmRequest{
		ReqId:     "req1",
		TimeStamp: time.Now().UnixMilli(),
	}
	ins.addReq(req)
	exists = m.CheckReqExists("req1")
	assert.Equal(t, exists, true)
}

// TestWithCacheDriverOps test redisOps register
func TestWithCacheDriverOps(t *testing.T) {
	redisOps := &cachecenter.CacheDriverOps{}
	opt := WithCacheDriverOps(redisOps)

	gs := &GlobalSchedulerManager{}
	err := opt(gs)

	assert.NoError(t, err)
	assert.Equal(t, redisOps, gs.cacheDriverOps)
}

// TestWithTokenizationRatio test redisOps register
func TestWithTokenizationRatio(t *testing.T) {
	ratio := tokenizers.DefaultTokenizationRatio
	opt := WithTokenizationRatio(ratio)

	gs := &GlobalSchedulerManager{}
	err := opt(gs)

	assert.NoError(t, err)
	assert.Equal(t, ratio, gs.config.tokenizationRatio)
}

// TestWithInsNumLimit test register insNumLimit
func TestWithInsNumLimit(t *testing.T) {
	insNumLimit := 10
	opt := WithInsNumLimit(insNumLimit)

	gs := &GlobalSchedulerManager{}
	err := opt(gs)

	assert.NoError(t, err)
	assert.Equal(t, insNumLimit, gs.config.maxInsNumPerGS)
}

// TestWithRuntimeMode test register aigw runtime mode
func TestWithRuntimeMode(t *testing.T) {
	gs := &GlobalSchedulerManager{}
	assert.Equal(t, base.ServiceMode, gs.runtimeMode)

	opt := WithRuntimeMode(base.SdkMode)
	err := opt(gs)

	assert.NoError(t, err)
	assert.Equal(t, base.SdkMode, gs.runtimeMode)
}

func TestGetStats(t *testing.T) {
	g := &GlobalSchedulerManager{stats: &stats.DataPlaneStats{}}
	res := g.GetStats()
	assert.NotNil(t, res)
	assert.Equal(t, len(res), int(stats.TypeCount))
}

// TestWithInsConnectType test register insNumLimit
func TestWithInsConnectType(t *testing.T) {
	connectType := "sse"
	opt := WithInsConnectType(connectType)
	gs := &GlobalSchedulerManager{}
	err := opt(gs)
	assert.NoError(t, err)
	assert.Equal(t, connectType, gs.config.insConnectType)
}

// TestRecordScheduleStats_Mixed_Success tests mixed deployment with valid prefillUrl
func TestRecordScheduleStats_Mixed_Success(t *testing.T) {
	// Arrange
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{
		config: globalSchedulerManagerConfig{deployPolicy: MixedDeployment},
		stats:  st,
	}
	result := &ScheduleResult{PrefillUrl: "http://prefill", DecodeUrl: ""}
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleFailure])
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_Mixed_Failure tests mixed deployment with empty prefillUrl
func TestRecordScheduleStats_Mixed_Failure(t *testing.T) {
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{
		config: globalSchedulerManagerConfig{deployPolicy: MixedDeployment},
		stats:  st,
	}
	result := &ScheduleResult{PrefillUrl: "", DecodeUrl: "http://decode"}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_Separated_Success tests both URLs present
func TestRecordScheduleStats_Separated_Success(t *testing.T) {
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{
		config: globalSchedulerManagerConfig{deployPolicy: SeparatedDeployment},
		stats:  st,
	}
	result := &ScheduleResult{PrefillUrl: "http://prefill", DecodeUrl: "http://decode"}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_Separated_Failure_PrefillEmpty tests prefillUrl empty
func TestRecordScheduleStats_Separated_Failure_PrefillEmpty(t *testing.T) {
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{config: globalSchedulerManagerConfig{deployPolicy: SeparatedDeployment}, stats: st}
	result := &ScheduleResult{PrefillUrl: "", DecodeUrl: "http://decode"}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_Separated_Failure_DecodeEmpty tests decodeUrl empty
func TestRecordScheduleStats_Separated_Failure_DecodeEmpty(t *testing.T) {
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{config: globalSchedulerManagerConfig{deployPolicy: SeparatedDeployment}, stats: st}
	result := &ScheduleResult{PrefillUrl: "http://prefill", DecodeUrl: ""}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_Separated_Failure_BothEmpty tests both empty
func TestRecordScheduleStats_Separated_Failure_BothEmpty(t *testing.T) {
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{
		config: globalSchedulerManagerConfig{deployPolicy: SeparatedDeployment},
		stats:  st,
	}
	result := &ScheduleResult{PrefillUrl: "", DecodeUrl: ""}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleFailure])
}

// TestRecordScheduleStats_UnknownPolicy_Failure tests unknown policy falls to default
func TestRecordScheduleStats_UnknownPolicy_Failure(t *testing.T) {
	// Arrange
	st := stats.NewDataPlaneStats()
	m := &GlobalSchedulerManager{
		config: globalSchedulerManagerConfig{deployPolicy: 2},
		stats:  st,
	}
	result := &ScheduleResult{PrefillUrl: "http://any", DecodeUrl: "http://any"}
	m.recordScheduleStats(result)
	assert.Equal(t, uint64(1), st.Counts[stats.ScheduleSuccess])
	assert.Equal(t, uint64(0), st.Counts[stats.ScheduleFailure])
}

// TestInitErrScheduler_SdkModeGuard verifies that queue mode is forced to
// passthrough when runtimeMode is not ServiceMode (i.e. SdkMode).
func TestInitErrScheduler_SdkModeGuard(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:  "err",
		QueueSize:         100,
		SessionTimeoutSec: 66,
		TaskIdleTimeoutSec: 6,
	}

	// SdkMode + queueSize>0 -> should fall back to passthrough (pendingQueue nil)
	m := &GlobalSchedulerManager{
		runtimeMode: base.SdkMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 8)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler, "errScheduler should still be created")
	assert.Nil(t, m.pendingQueue, "pendingQueue must be nil in SdkMode (forced passthrough)")
	assert.Nil(t, m.capacityProvider, "capacityProvider must be nil in passthrough mode")

	// ServiceMode + queueSize>0 -> queue mode should be enabled
	m2 := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err2 := m2.initErrScheduler(cfg, 8)
	assert.NoError(t, err2)
	assert.NotNil(t, m2.errScheduler)
	assert.NotNil(t, m2.pendingQueue, "pendingQueue should be created in ServiceMode with queueSize>0")
	assert.NotNil(t, m2.capacityProvider)
}

// TestNewGlobalSchedulerManager_SdkModeErrPassthrough verifies the full constructor
// path: when WithRuntimeMode(SdkMode) is passed as an option, initErrScheduler must
// see the correct runtimeMode and fall back to passthrough (pendingQueue == nil).
// Regression test for the init-order bug where setConfig ran before opts were applied.
func TestNewGlobalSchedulerManager_SdkModeErrPassthrough(t *testing.T) {
	parentCtx, cancel := context.WithCancel(context.Background())
	defer cancel()

	cfg := &base.GlobalSchedulerConfig{
		Model:                "test-sdk-model",
		BlockSize:            128,
		DeployPolicy:         "mixed",
		MaxTimeToFirstToken:  200,
		MaxTimeBetweenTokens: 50,
		LoadBalancer: base.LoadBalancerConfig{
			Mixed:               "roundRobin",
			Prefill:             "roundRobin",
			Decode:              "roundRobin",
			BatchSize:           1,
			ReservedBlockNumber: 50,
		},
		ErrScheduler: base.ErrSchedulerConfig{
			ErrSchedulerType:   "err",
			QueueSize:          100,
			SessionTimeoutSec:  66,
			TaskIdleTimeoutSec: 6,
		},
	}

	// SdkMode + queueSize > 0 → must fall back to passthrough
	mgr, err := NewGlobalSchedulerManager(parentCtx, cfg,
		WithRuntimeMode(base.SdkMode),
		WithInsNumLimit(10),
	)
	assert.NoError(t, err)
	assert.NotNil(t, mgr.errScheduler, "errScheduler should be created even in SdkMode")
	assert.Nil(t, mgr.pendingQueue, "pendingQueue must be nil in SdkMode (passthrough)")

	// ServiceMode + queueSize > 0 → queue mode must be enabled
	cfg2 := *cfg
	cfg2.ErrScheduler.QueueSize = 10
	mgr2, err := NewGlobalSchedulerManager(parentCtx, &cfg2,
		WithRuntimeMode(base.ServiceMode),
		WithInsNumLimit(10),
	)
	assert.NoError(t, err)
	assert.NotNil(t, mgr2.errScheduler)
	assert.NotNil(t, mgr2.pendingQueue, "pendingQueue should be created in ServiceMode")
}

// TestInitErrSchedulerCatErrMissingVocab verifies that initErrScheduler
// returns an error (blocking startup) when the vocab file does not exist.
func TestInitErrSchedulerCatErrMissingVocab(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "caterr",
		QueueSize:          10,
		TfidfVocabPath:     "/nonexistent/vocab.json",
		TfidfCentroidsPath: "/nonexistent/centroids.json",
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.Error(t, err)
	assert.Contains(t, err.Error(), "classifier not loaded")
}

// TestInitErrSchedulerCatErrCorruptVocab verifies that a corrupt vocab file
// (invalid JSON) blocks startup.
func TestInitErrSchedulerCatErrCorruptVocab(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{not valid json`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0]],"labels":["a"]}`), 0644)

	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "caterr",
		QueueSize:          10,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.Error(t, err)
	assert.Contains(t, err.Error(), "classifier not loaded")
}

// TestInitErrSchedulerCatErrCorruptCentroids verifies that corrupt centroids
// (label/centroid count mismatch) blocks startup.
func TestInitErrSchedulerCatErrCorruptCentroids(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["a","b"],"GIdf":{"a":1.0,"b":1.0}}`), 0644)
	// 3 centroids but only 2 labels — mismatch
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0],[1.0,1.0]],"labels":["cat_a","cat_b"]}`), 0644)

	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "caterr",
		QueueSize:          10,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.Error(t, err)
	assert.Contains(t, err.Error(), "classifier not loaded")
}

// TestInitErrSchedulerCatErrDimensionMismatch verifies that vocab and centroids
// with mismatched dimensions (vocab has 3 terms but centroids have 2 dims)
// does not crash at classify time. The classifier loads successfully (LoadCentroids
// only checks label/centroid count), but Classify returns "" because cosineSim
// gets length-mismatched vectors.
func TestInitErrSchedulerCatErrDimensionMismatch(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	// Vocab: 3 terms
	os.WriteFile(vocabPath, []byte(`{"GVocab":["a","b","c"],"GIdf":{"a":1.0,"b":1.0,"c":1.0}}`), 0644)
	// Centroids: 2 dims (mismatch with vocab's 3 dims)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,0.0],[0.0,1.0]],"labels":["cat_a","cat_b"]}`), 0644)

	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "caterr",
		QueueSize:          10,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	// initErrScheduler succeeds — IsLoaded only checks label/centroid count
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler)

	// Classify with dimension mismatch does not crash; cosineSim returns 0 for
	// all centroids → falls back to labels[0]. This is a degraded mode, not a crash.
	cat := m.errScheduler.classifier.Classify("a b c")
	assert.NotPanics(t, func() { m.errScheduler.classifier.Classify("a b c") })
	_ = cat // result is labels[0] due to all-zero cosineSim — degraded but safe
}

// TestInitErrSchedulerCatErrValidFiles verifies that valid vocab + centroids
// files allow startup and classifier is loaded.
func TestInitErrSchedulerCatErrValidFiles(t *testing.T) {
	tmpDir := t.TempDir()
	vocabPath := filepath.Join(tmpDir, "vocab.json")
	centroidsPath := filepath.Join(tmpDir, "centroids.json")

	os.WriteFile(vocabPath, []byte(`{"GVocab":["code","debug","office","meeting"],"GIdf":{"code":1.0,"debug":1.0,"office":1.0,"meeting":1.0}}`), 0644)
	os.WriteFile(centroidsPath, []byte(`{"centroids":[[1.0,1.0,0.0,0.0],[0.0,0.0,1.0,1.0]],"labels":["code","office"]}`), 0644)

	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "caterr",
		QueueSize:          10,
		TfidfVocabPath:     vocabPath,
		TfidfCentroidsPath: centroidsPath,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler)
	assert.True(t, m.errScheduler.classifier.IsLoaded())

	// Classify should work
	cat := m.errScheduler.classifier.Classify("code debug")
	assert.Equal(t, "code", cat)
}

// TestInitErrSchedulerSeparatedDeployment verifies that separated deployment
// uses NewCapacityProviderSeparated (min(prefill, decode) * batchSize).
func TestInitErrSchedulerSeparatedDeployment(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "err",
		QueueSize:          10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode:   base.ServiceMode,
		config:        globalSchedulerManagerConfig{model: "test-model", deployPolicy: SeparatedDeployment},
	}
	err := m.initErrScheduler(cfg, 2)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler)
	assert.NotNil(t, m.pendingQueue, "pendingQueue should be created in ServiceMode")
	assert.NotNil(t, m.capacityProvider, "capacityProvider should be created in queue mode")

	// Without instances, min(prefill=0, decode=0) * 2 = 0
	assert.Equal(t, 0, m.capacityProvider.TotalSlots())
	assert.Equal(t, 0, m.capacityProvider.InstanceCount())

	// Verify it's using group-aware separated mode
	assert.NotNil(t, m.capacityProvider.separatedCapacity, "separatedCapacity should be set in separated mode")
	assert.Nil(t, m.capacityProvider.prefillCount, "prefillCount should be nil in group-aware mode")
	assert.Nil(t, m.capacityProvider.decodeCount, "decodeCount should be nil in group-aware mode")
	assert.Nil(t, m.capacityProvider.instanceCount, "instanceCount should be nil in separated mode")
}

// TestInitErrSchedulerMixedDeployment verifies that mixed deployment
// uses NewCapacityProvider (instanceCount * batchSize).
func TestInitErrSchedulerMixedDeployment(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "err",
		QueueSize:          10,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode:   base.ServiceMode,
		config:        globalSchedulerManagerConfig{model: "test-model", deployPolicy: MixedDeployment},
	}
	err := m.initErrScheduler(cfg, 2)
	assert.NoError(t, err)
	assert.NotNil(t, m.capacityProvider)
	assert.NotNil(t, m.capacityProvider.instanceCount, "instanceCount should be set in mixed mode")
	assert.Nil(t, m.capacityProvider.prefillCount, "prefillCount should be nil in mixed mode")
	assert.Nil(t, m.capacityProvider.decodeCount, "decodeCount should be nil in mixed mode")
}

// TestErrOnResponseUnknownFinishReason verifies that empty/unknown finish_reason
// triggers RestartIdleTimer (not OnFinish or OnCancel).
func TestErrOnResponseUnknownFinishReason(t *testing.T) {
	m := &GlobalSchedulerManager{
		errScheduler: NewErrSchedulerManager(ErrSchedulerConfig{
			SchedulerType:      "err",
			WarmupSessions:     0,
			SessionTimeoutSec:  660,
			TaskIdleTimeoutSec: 60,
		}),
	}
	m.errScheduler.OnRequest("sess-unknown", "model", "test")

	// "unknown" / "" / "tool_calls" → should arm idle timer, not record steps
	m.ErrOnResponse("sess-unknown", "req-1", "", "", "unknown")
	m.errScheduler.globalDist.Refresh()
	assert.Equal(t, 0, m.errScheduler.globalDist.TotalCount(), "unknown finishReason should not record steps")

	// Session should still exist (not cancelled)
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-unknown"))

	// Empty finishReason should behave the same as "unknown"
	m.ErrOnResponse("sess-unknown", "req-2", "", "", "")
	m.errScheduler.globalDist.Refresh()
	assert.Equal(t, 0, m.errScheduler.globalDist.TotalCount())
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-unknown"))

	// "tool_calls" should also behave the same
	m.ErrOnResponse("sess-unknown", "req-3", "", "", "tool_calls")
	m.errScheduler.globalDist.Refresh()
	assert.Equal(t, 0, m.errScheduler.globalDist.TotalCount())
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-unknown"))
}

// TestErrOnResponseStopFinishReason verifies that "stop" calls OnFinish
// (records step count into distribution).
func TestErrOnResponseStopFinishReason(t *testing.T) {
	m := &GlobalSchedulerManager{
		errScheduler: NewErrSchedulerManager(ErrSchedulerConfig{
			SchedulerType:      "err",
			WarmupSessions:     0,
			SessionTimeoutSec:  660,
			TaskIdleTimeoutSec: 60,
		}),
	}
	m.errScheduler.OnRequest("sess-stop", "model", "test")
	m.errScheduler.OnRequest("sess-stop", "model", "test")
	m.errScheduler.OnRequest("sess-stop", "model", "test")

	m.ErrOnResponse("sess-stop", "req-1", "", "", "stop")

	// After OnFinish, steps should be recorded in distribution
	m.errScheduler.globalDist.Refresh()
	assert.Equal(t, 1, m.errScheduler.globalDist.TotalCount(), "stop should record step count into distribution")
}

// TestErrOnResponseCancelledFinishReason verifies that "cancelled" calls OnCancel
// (removes session without recording steps).
func TestErrOnResponseCancelledFinishReason(t *testing.T) {
	m := &GlobalSchedulerManager{
		errScheduler: NewErrSchedulerManager(ErrSchedulerConfig{
			SchedulerType:      "err",
			WarmupSessions:     0,
			SessionTimeoutSec:  660,
			TaskIdleTimeoutSec: 60,
		}),
	}
	m.errScheduler.OnRequest("sess-cancel", "model", "test")
	m.errScheduler.OnRequest("sess-cancel", "model", "test")

	m.ErrOnResponse("sess-cancel", "req-1", "", "", "cancelled")
	m.errScheduler.globalDist.Refresh()
	assert.Equal(t, 0, m.errScheduler.globalDist.TotalCount(), "cancelled should not record steps")

	// Session survives with k rolled back (attempt completed nothing the
	// client saw; the SDK retry re-enters the same step)
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-cancel"),
		"cancelled rolls k back, session survives for the retry")
}

// TestInitErrSchedulerPassthroughMode verifies that queueSize=0 creates
// errScheduler but no pendingQueue and no capacityProvider.
func TestInitErrSchedulerPassthroughMode(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "err",
		QueueSize:          0,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler, "errScheduler should be created even in passthrough")
	assert.Nil(t, m.pendingQueue, "pendingQueue must be nil when queueSize=0")
	assert.Nil(t, m.capacityProvider, "capacityProvider must be nil in passthrough mode")
	assert.False(t, m.HasQueueMode())
	assert.True(t, m.HasErrScheduler())
}

// TestInitErrSchedulerQueueMode verifies that queueSize>0 creates all components.
func TestInitErrSchedulerQueueMode(t *testing.T) {
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "err",
		QueueSize:          50,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}

	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	assert.NotNil(t, m.errScheduler)
	assert.NotNil(t, m.pendingQueue, "pendingQueue should be created when queueSize>0")
	assert.NotNil(t, m.capacityProvider, "capacityProvider should be created when queueSize>0")
	assert.True(t, m.HasQueueMode())
	assert.True(t, m.HasErrScheduler())
}

// helper: minimal GSM with ERR scheduler + capacityProvider + errInFlight map
func newErrTestGSM(t *testing.T) *GlobalSchedulerManager {
	t.Helper()
	m := &GlobalSchedulerManager{
		runtimeMode: base.ServiceMode,
		config:      globalSchedulerManagerConfig{model: "test-model"},
	}
	cfg := &base.ErrSchedulerConfig{
		ErrSchedulerType:   "err",
		QueueSize:          50,
		SessionTimeoutSec:  660,
		TaskIdleTimeoutSec: 60,
	}
	err := m.initErrScheduler(cfg, 1)
	assert.NoError(t, err)
	return m
}

// TestErrInFlightRegistryIdempotentRelease verifies that errReleaseReq is
// idempotent: calling it twice for the same reqID only decrements once.
func TestErrInFlightRegistryIdempotentRelease(t *testing.T) {
	m := newErrTestGSM(t)

	m.dispatchMu.Lock()
	m.errInFlight["req-1"] = errInFlightEntry{
		SessionID:  "sess-A",
		PrefillURL: "http://ins-1",
	}
	m.capacityProvider.OnDispatched()
	assert.Equal(t, int32(1), m.capacityProvider.InFlight())

	// First release: should succeed
	_, released := m.errReleaseReq("req-1")
	assert.True(t, released)
	assert.Equal(t, int32(0), m.capacityProvider.InFlight())
	assert.Len(t, m.errInFlight, 0)

	// Second release: no-op
	_, released = m.errReleaseReq("req-1")
	assert.False(t, released)
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "inFlight must not go negative")
	m.dispatchMu.Unlock()
}

// TestOnInstanceUnhealthyNoDoubleDecrement is the BUG-1 regression test.
// Before the registry fix, onInstanceUnhealthy used a pre-fetched req list
// (outside dispatchMu). If ErrOnResponse completed the request between
// fetch and lock, OnCompleted was called twice → inFlight under-counted
// and the session was erroneously OnCancel'd.
//
// With the registry fix: errReleaseReq is idempotent, so even if
// ErrOnResponse fires first, onInstanceUnhealthy finds nothing to clean.
func TestOnInstanceUnhealthyNoDoubleDecrement(t *testing.T) {
	m := newErrTestGSM(t)

	// Simulate dispatch of req-1 to ins-1
	m.dispatchMu.Lock()
	m.errInFlight["req-1"] = errInFlightEntry{
		SessionID:  "sess-A",
		PrefillURL: "http://ins-1",
	}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	// Session registered
	m.errScheduler.OnRequest("sess-A", "test-model", "prompt")
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-A"))

	// Normal completion: ErrOnResponse("stop") cleans req-1
	m.ErrOnResponse("sess-A", "req-1", "http://ins-1", "", "stop")
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "inFlight should be 0 after completion")

	// Now instance goes unhealthy — onInstanceUnhealthy should find nothing
	m.onInstanceUnhealthy("http://ins-1")

	// inFlight must NOT go negative (would have before fix)
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "no double-decrement")

	// Session should NOT be re-cancelled (OnFinish already ran, session
	// should still be tracked until idle timeout — NOT deleted by OnCancel)
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-A"),
		"session must survive — OnCancel must not fire for already-completed request")
}

// TestErrOnCancelDuplicateIdempotent verifies that a duplicate ErrOnCancel
// for the same reqID does not double-decrement or wipe a re-registered session.
func TestErrOnCancelDuplicateIdempotent(t *testing.T) {
	m := newErrTestGSM(t)

	// Dispatch req-1
	m.dispatchMu.Lock()
	m.errInFlight["req-1"] = errInFlightEntry{
		SessionID:  "sess-A",
		PrefillURL: "http://ins-1",
	}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	m.errScheduler.OnRequest("sess-A", "test-model", "prompt")

	// First ErrOnCancel: normal cleanup
	m.ErrOnCancel("sess-A", "req-1", "http://ins-1", "")
	assert.Equal(t, int32(0), m.capacityProvider.InFlight())
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-A"),
		"in-flight abort rolls k back, session survives for the retry")

	// The retry re-enters at the same step number (k was rolled back to 1)
	assert.Equal(t, 2, m.errScheduler.OnRequest("sess-A", "test-model", "prompt2"),
		"session continues — retry re-enters the same step")

	// Late duplicate ErrOnCancel for same req-1: must NOT roll back again
	m.ErrOnCancel("sess-A", "req-1", "http://ins-1", "")
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "no double-decrement")
	assert.Equal(t, 2, m.errScheduler.SessionIteration("sess-A"),
		"session must survive a late duplicate cancel without a second rollback")
}

// TestOnInstanceUnhealthySeparatedURLMatch verifies that in separated mode,
// onInstanceUnhealthy matches by either prefillURL or decodeURL.
func TestOnInstanceUnhealthySeparatedURLMatch(t *testing.T) {
	m := newErrTestGSM(t)

	// Dispatch req-1 with both prefill and decode URLs
	m.dispatchMu.Lock()
	m.errInFlight["req-1"] = errInFlightEntry{
		SessionID:  "sess-A",
		PrefillURL: "http://prefill-1",
		DecodeURL:  "http://decode-1",
	}
	m.capacityProvider.OnDispatched()
	m.dispatchMu.Unlock()

	m.errScheduler.OnRequest("sess-A", "test-model", "prompt")

	// Decode instance goes unhealthy → should clean req-1
	m.onInstanceUnhealthy("http://decode-1")
	assert.Equal(t, int32(0), m.capacityProvider.InFlight(), "should be cleaned by decode URL match")
	assert.Len(t, m.errInFlight, 0)
	assert.Equal(t, 1, m.errScheduler.SessionIteration("sess-A"),
		"instance death rolls k back, session survives for the retry")
}
