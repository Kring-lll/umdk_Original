/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: definitions of types for AIGW core.
 * Create: 2025-06-11
 */

// Package base contains the core functions for AIGW.
package base

import (
	"encoding/json"

	"huawei.com/aigw/internal/renderclient"
)

// OpenAiMessage specified the input format of OpenAI
type OpenAiMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

// OpenAiRequest specified the input format of OpenAI http server
type OpenAiRequest struct {
	UUID     string          `json:"uuid"`
	Model    string          `json:"model"`
	Messages []OpenAiMessage `json:"messages"`
}

// GetSuggestionOut specified the out parameters for GetSuggestion.
type GetSuggestionOut struct {
	TargetPrefillUrl string `json:"targetPrefill"`
	TargetDecodeUrl  string `json:"targetDecode"`
	DpRank           *int   `json:"dpRank,omitempty"`   // DP rank for data parallel routing
	Priority         *int   `json:"priority,omitempty"` // ERR scheduling priority
}

// RegisterInstanceIn specified the input parameters for RegisterInstance.
type RegisterInstanceIn struct {
	Name  string `json:"name"`
	Model string `json:"model"`
	IP    string `json:"instanceIp"`
	Port  string `json:"port"`
	Role  string `json:"role"`

	GroupID string `json:"groupID"`
	DpRank  int    `json:"dpRank"` // DP rank for data parallel routing
}

// UnregisterInstanceIn specified the input parameters for UnregisterInstance.
type UnregisterInstanceIn struct {
	Model  string `json:"model"`
	IP     string `json:"instanceIp"`
	Port   string `json:"port"`
	DpRank int    `json:"dpRank"` // DP rank for data parallel routing
}

// ZookeeperConfig struct contains configuration details for Zookeeper
type ZookeeperConfig struct {
	Address               string `json:"address"`
	AclScheme             string `json:"aclScheme"`
	ConnectTimeout        int    `json:"connectTimeout"`
	SessionTimeout        int    `json:"sessionTimeout"`
	InferenceInstancePath string `json:"inferenceInstancePath"`
	ScheduleServicePath   string `json:"scheduleServicePath"`
	EnableTls             bool   `json:"enableTls"`
	CaFile                string `json:"tlsCaFile"`
	CrtFile               string `json:"tlsCrtFile"`
	ServerName            string `json:"tlsServerName"`
}

// DiscoveryConfig struct contains configuration for service discovery
type DiscoveryConfig struct {
	Type                   string `json:"type"`                   // Discovery type: "k8s", "dns", "zk"
	KubeconfigPath         string `json:"kubeconfigPath"`         // K8s kubeconfig path (optional, use in-cluster config if empty)
	Namespace              string `json:"namespace"`              // K8s namespace to watch
	ResyncPeriod           int    `json:"resyncPeriod"`           // Resync period in seconds
	Enable                 bool   `json:"enable"`                 // Enable service discovery
	SkipInstanceConnection bool   `json:"skipInstanceConnection"` // Skip connecting to instances during registration (for testing)
}

// ProxyConfig struct contains configuration for request proxy/forwarding
type ProxyConfig struct {
	Timeout           int                  `json:"timeout"`           // Request timeout in seconds
	MaxRetry          int                  `json:"maxRetry"`          // Maximum retry attempts
	RetryBaseInterval int                  `json:"retryBaseInterval"` // Retry base interval in milliseconds
	RetryMaxInterval  int                  `json:"retryMaxInterval"`  // Retry max interval in milliseconds
	Enable            bool                 `json:"enable"`            // Enable request forwarding
	CircuitBreaker    CircuitBreakerConfig `json:"circuitBreaker"`    // Circuit breaker configuration
	// Stream timeout budgets, mirroring the openjiuwen SDK defaults so aigw
	// gives up roughly when the last possible reader does:
	// firstChunkTimeoutSec ← stream_first_chunk_timeout (300s),
	// chunkIntervalTimeoutSec ← stream_idle_timeout (60s).
	// Deployments that override the client budgets (e.g. jiuwenswarm's
	// config.yaml raises idle to 120s) must raise these to match.
	//
	// FirstChunkTimeoutSec: max wait for the FIRST byte of an engine
	// response. The engine may legitimately queue and prefill for minutes
	// before the first token.
	//
	// ChunkIntervalTimeoutSec: max gap between tokens, spent waiting in
	// either direction — a silent engine after the first byte (dead
	// mid-stream) or a client write stuck this long (abandoned without
	// closing, TCP window full).
	FirstChunkTimeoutSec    int `json:"firstChunkTimeoutSec"`
	ChunkIntervalTimeoutSec int `json:"chunkIntervalTimeoutSec"`
}

// LoraConfig contains static LoRA registry configuration.
type LoraConfig struct {
	Root             string `json:"root"`
	StatePath        string `json:"statePath"`
	OperationTimeout string `json:"operationTimeout"`
}

// OnlineRLConfig contains static RL child-process configuration.
type OnlineRLConfig struct {
	Command        []string `json:"command"`
	ConfigPath     string   `json:"configPath"`
	Endpoint       string   `json:"endpoint"`
	HookTimeout    string   `json:"hookTimeout"`
	HookRetries    int      `json:"hookRetries"`
	DrainTimeout   string   `json:"drainTimeout"`
	ControlTimeout string   `json:"controlTimeout"`
}

// CircuitBreakerConfig struct contains configuration for circuit breaker
type CircuitBreakerConfig struct {
	Enabled          bool `json:"enabled"`          // Enable circuit breaker
	FailureThreshold int  `json:"failureThreshold"` // Number of failures before opening circuit
	SuccessThreshold int  `json:"successThreshold"` // Number of successes before closing circuit
	Timeout          int  `json:"timeout"`          // Circuit open timeout in seconds
}

// MonitorConfig struct contains configuration detail for Monitor
type MonitorConfig struct {
	Address     string `json:"address"`
	AlarmPath   string `json:"alarmPath"`
	ServiceName string `json:"serviceName"`
	Version     string `json:"version"`
	BusinessId  string `json:"businessId"`
}

// LightgbmConfig struct contains configuration for the LightGBM
type LightgbmConfig struct {
	ClassifierFile string `json:"classifierFile"`
	VectorizerFile string `json:"vectorizerFile"`
}

// PredictorConfig struct contains configuration for the predictor
type PredictorConfig struct {
	PredictType string         `json:"predictType"`
	Lightgbm    LightgbmConfig `json:"lightgbm"`
}

// TokenizerConfig struct contains configuration for the tokenizer
type TokenizerConfig struct {
	TokenizeModelName string `json:"tokenizeModelName"`
	ConfigPath        string `json:"configPath"`
	TokenizerType     string `json:"tokenizerType"`
}

// LoadBalancerConfig struct contains configuration for the load balancer
type LoadBalancerConfig struct {
	Mixed               string `json:"mixed"`
	Prefill             string `json:"prefill"`
	Decode              string `json:"decode"`
	BatchSize           int    `json:"batchSize"`
	ReservedBlockNumber int    `json:"reservedBlockNumber"`
	MinMatchedLength    int    `json:"minMatchedLength"`
	PowerOfTwo          bool   `json:"enablePowerOfTwo"`
	PretrainTTFTPath    string `json:"pretrainTTFTPath"`
	// Consistent hash load balancer configuration
	VirtualNodes int `json:"virtualNodes"` // Number of virtual nodes per worker (default: 160)
	FallbackNum  int `json:"fallbackNum"`  // Number of fallback nodes on hash miss (default: 3)
	DpSize       int `json:"dpSize"`       // Data parallel size for DP-aware routing
}

// ErrSchedulerConfig holds ERR-scheduler-specific parameters, separate from
// LoadBalancerConfig. BatchSize (shared by LBs and ERR capacity) stays in
// LoadBalancerConfig and is passed into initErrScheduler separately.
type ErrSchedulerConfig struct {
	ErrSchedulerType   string `json:"errSchedulerType"`   // "mej", "err", "caterr", "fifo", "" or "none" = disabled
	QueueSize          int    `json:"queueSize"`          // pending queue max size (0 = passthrough mode, >0 = queue mode)
	WarmupSessions     int    `json:"warmupSessions"`     // total observations threshold for warmup completion (0 = default 100; negative = total gate disabled)
	WarmupPerCat       int    `json:"warmupPerCat"`       // per-category observations threshold, CatERR only (0 = default 10; negative = per-category gate disabled)
	WindowSize         int    `json:"windowSize"`         // EmpiricalDist sliding window size (0 = unbounded, default: 5000)
	TrainDataPath      string `json:"trainDataPath"`      // path to pre-trained distributions JSON (CatERR only; ERR can warm up online)
	TfidfVocabPath     string `json:"tfidfVocabPath"`     // path to TF-IDF vocab JSON (CatERR)
	TfidfCentroidsPath string `json:"tfidfCentroidsPath"` // path to K-means centroids JSON (CatERR)
	SessionTimeoutSec  int    `json:"sessionTimeoutSec"`  // session idle timeout after tool_calls/other, seconds (default: 660)
	TaskIdleTimeoutSec int    `json:"taskIdleTimeoutSec"` // session idle timeout after stop, seconds (default: 660)
	QueueBoostSec      int    `json:"queueBoostSec"`      // starvation floor: force queued requests to top priority after this many seconds (default: 240; 0 = default; negative = disabled)
}

// KvAdmission mode values.
const (
	KvAdmissionOff    = "off"
	KvAdmissionSignal = "signal"
)

// KvAdmissionConfig configures engine-signal admission control: dispatch is
// gated on the target engine's own waiting queue (observed via the motor
// coordinator's metrics endpoint) instead of aigw-side counting. It replaces
// the original double gate (global capacity provider + LB ReqNum<batchSize)
// whenever active, and requires ERR queue mode.
type KvAdmissionConfig struct {
	Mode string `json:"mode"` // "signal" | "off" (default; original gates stay)
	// MetricsURL is the motor coordinator observation endpoint
	// (e.g. http://127.0.0.1:1027/metrics?type=instance); explicit full URL,
	// not derived from the instance URL (obs and infer are separately exposed).
	MetricsURL           string `json:"metricsUrl"`
	PollMs               int    `json:"pollMs"`               // signal poll cadence (default 1000)
	BurstCapPerCycle     int    `json:"burstCapPerCycle"`     // max admits per instance per poll cycle (0 = auto: one per free engine, min across prefill/decode roles; explicit value wins)
	HysteresisCycles     int    `json:"hysteresisCycles"`     // clear cycles required before resuming after waiting>0 (default 1)
	StaleAfterMs         int    `json:"staleAfterMs"`         // signal age treated as fresh (default 8000: motor cache 3s + poll offset)
	RecoverHealthyCycles int    `json:"recoverHealthyCycles"` // consecutive good polls before trusting signals (default 2)
	QuotaAgeFallbackMs   int    `json:"quotaAgeFallbackMs"`   // force quota reset after this long on a static engine, ms (default 3000 = motor refresh cadence)
}

// GlobalSchedulerConfig struct contains configuration for a global scheduler
type GlobalSchedulerConfig struct {
	Model                string             `json:"model"`
	BlockSize            int                `json:"blockSize"`
	DeployPolicy         string             `json:"deployPolicy"`
	MaxTimeToFirstToken  float64            `json:"maxTimeToFirstToken"`
	MaxTimeBetweenTokens float64            `json:"maxTimeBetweenTokens"`
	TokenizeModelName    string             `json:"tokenizeModelName"`
	LoadBalancer         LoadBalancerConfig `json:"loadBalancer"`
	ErrScheduler         ErrSchedulerConfig `json:"errScheduler"`
	KvAdmission          KvAdmissionConfig  `json:"kvAdmission"`
	// Mode selects scheduling path: "instance" (default when empty) | "provider" | "scheduler".
	// "scheduler" = forward to a downstream Scheduler component (northbound proxy);
	// Adapter does not schedule itself, only forwards request + agent_hint.
	Mode string `json:"mode,omitempty"`
	// ProviderPool holds config for mode=provider.
	ProviderPool *ProviderPoolConfig `json:"providerPool,omitempty"`
	// SchedulerTarget holds config for mode=scheduler.
	SchedulerTarget        *SchedulerTargetConfig `json:"schedulerTarget,omitempty"`
	InsConnectType         string                 `json:"instanceConnectType"`
	SkipInstanceConnection bool                   `json:"skipInstanceConnection"` // Skip connecting to instances during registration (for testing with mock instances)

	CacheRefreshIntervalMs uint32  `json:"cacheRefreshIntervalMs"`
	TokenizationRatio      float64 `json:"tokenizationRatio"`

	// RenderClient configures the render service client for prefix cache tokenization
	RenderClient renderclient.RenderClientConfig `json:"renderClient"`
}

// ProviderPoolConfig configures a provider (SaaS API) pool for mode=provider.
type ProviderPoolConfig struct {
	Strategy        string             `json:"strategy"`
	StrategyOptions map[string]any     `json:"strategyOptions,omitempty"`
	Cooldown        *CooldownConfig    `json:"cooldown,omitempty"`
	Retry           *RetryConfig       `json:"retry,omitempty"`
	Deployments     []DeploymentConfig `json:"deployments"`
}

// SchedulerTargetConfig configures a downstream Scheduler endpoint for mode=scheduler.
// Adapter forwards the client request body unmodified (agent_hint passed through as-is;
// no session fallback — the Scheduler resolves session_id from headers) to BaseURL +
// the fixed OpenAI completion route. Auth is injected as a Bearer token; TLS is optional.
type SchedulerTargetConfig struct {
	BaseURL string    `json:"baseURL"` // e.g. "http://scheduler-coordinator:8080"
	APIKey  string    `json:"apiKey"`  // Bearer token injected as Authorization
	TLS     TLSConfig `json:"tls,omitempty"`
}

// TLSConfig configures TLS for an outbound connection. Field names mirror the
// ZookeeperConfig TLS fields (enableTls/tlsCaFile/tlsCrtFile/tlsServerName) for
// consistency with existing config conventions.
type TLSConfig struct {
	EnableTLS  bool   `json:"enableTls"`
	CAFile     string `json:"tlsCaFile"`
	CrtFile    string `json:"tlsCrtFile"`
	ServerName string `json:"tlsServerName"`
}

// CooldownConfig configures per-endpoint cooldown behavior.
type CooldownConfig struct {
	FailureThreshold     int `json:"failureThreshold"`
	DurationSec          int `json:"durationSec"`
	RateLimitDurationSec int `json:"rateLimitDurationSec"`
	Auth401FloorSec      int `json:"auth401FloorSec"`
}

// RetryConfig configures cross-endpoint failover and per-endpoint retry caps.
type RetryConfig struct {
	MaxFailoverEndpoints  int `json:"maxFailoverEndpoints"`
	MaxRetriesPerEndpoint int `json:"maxRetriesPerEndpoint"`
}

// DeploymentConfig is one provider endpoint in a ProviderPoolConfig.
type DeploymentConfig struct {
	ID               string   `json:"id,omitempty"`
	Provider         string   `json:"provider"`
	APIBase          string   `json:"apiBase"`
	APIKey           string   `json:"apiKey"`
	Model            string   `json:"model,omitempty"` // upstream model name; the request body's "model" is rewritten to this when set
	TPM              int      `json:"tpm,omitempty"`
	RPM              int      `json:"rpm,omitempty"`
	Tags             []string `json:"tags,omitempty"`
	Timeout          int      `json:"timeout,omitempty"`
	VerifySSL        *bool    `json:"verifySsl,omitempty"`
	AuthHeaderName   string   `json:"authHeaderName,omitempty"`
	AuthHeaderPrefix string   `json:"authHeaderPrefix,omitempty"`
}

// GlobalConfig is the global config of AIGW.
type GlobalConfig struct {
	Host     string `json:"host"`
	Port     string `json:"port"`
	LogPath  string `json:"logPath"`
	LogLevel string `json:"logLevel"`

	CryptoSock             string `json:"cryptoSock"`
	SnapshotUpdateInterval int    `json:"snapshotUpdateInterval"`
	SecuritySchema         string `json:"securitySchema"`
	ReqTimeout             int64  `json:"reqTimeout"`
}

// Limits Specification Limitations
type Limits struct {
	TotalInsNum    int `json:"totalInsNum"`
	InsNumPerModel int `json:"insNumPerModel"`
	ModelNum       int `json:"modelNum"`
	Concurrency    int `json:"concurrency"`
	MaxPromptRunes int `json:"maxPromptRunes"`
}

// AigwConfig is the config for AIGW.
type AigwConfig struct {
	MonitorConfig  MonitorConfig           `json:"monitor"`
	ZkConfig       ZookeeperConfig         `json:"zookeeper"`
	DataSyncConfig DataSyncConfig          `json:"dataSync"`
	Predictor      PredictorConfig         `json:"predictor"`
	GsConfigs      []GlobalSchedulerConfig `json:"globalSchedulers"`
	Tokenizers     []TokenizerConfig       `json:"tokenizers"`
	// SchedulerNorthbound is the global downstream Scheduler target (northbound passthrough mode).
	// When set, any model not explicitly registered as instance/provider is forwarded to this
	// Scheduler as-is (body.model passes through untouched). Decouples the routing key from the
	// request model name so one Scheduler can serve many models with many instances.
	SchedulerNorthbound *SchedulerTargetConfig `json:"schedulerNorthbound,omitempty"`
	GlobalConfig        GlobalConfig           `json:"global"`
	Limits              Limits                 `json:"limits"`
	Discovery           DiscoveryConfig        `json:"discovery"` // Service discovery configuration
	Proxy               ProxyConfig            `json:"proxy"`     // Request proxy/forwarding configuration
	Kvc                 KvcConfig              `json:"kvc"`       // KVC management (Phase 2; ServiceMode only)
	Lora                LoraConfig             `json:"lora"`
	OnlineRL            OnlineRLConfig         `json:"onlineRL"`
}

// String is the string method of AigwConfig
func (cfg *AigwConfig) String() string {
	cfgJson, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return ""
	}
	return string(cfgJson)
}

// AigwAllStats is the stats info for Aigw
type AigwAllStats struct {
	StatsSlice []*StatsEntry `json:"stats"`
}

// StatsEntry is the stats entry for one global scheduler
type StatsEntry struct {
	ModelName string            `json:"modelName"`
	Counts    map[string]uint64 `json:"counts"`
}

// CryptoData contains the data of crypto for AIGW
type CryptoData struct {
	ApiHmacKey        string `json:"apiHmacKey"`
	ApiAesKey         string `json:"apiAesKey"`
	InsHmacKey        string `json:"insHmacKey"`
	InsAesKey         string `json:"insAesKey"`
	MonitorHmacKey    string `json:"monitorHmacKey"`
	DataSyncHmacKey   string `json:"dataSyncHmacKey"`
	ZookeeperUser     string `json:"zookeeperUser"`
	ZookeeperPassword string `json:"zookeeperPassword"`
	ZookeeperTlsKey   string `json:"zookeeperTlsKey"`
}

// DataSyncConfig is the stats info for datasync
type DataSyncConfig struct {
	Address  string `json:"address"`
	Path     string `json:"path"`
	Interval int    `json:"interval"`
}

// RuntimeMode is the mode of aigw runtime
type RuntimeMode int

const (
	// ServiceMode is the standalone service mode for aigw
	ServiceMode RuntimeMode = iota
	// SdkMode it sdk mode for aigw
	SdkMode
)
