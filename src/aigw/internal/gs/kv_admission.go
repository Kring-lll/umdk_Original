/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * Description: Engine-signal admission control — gates dispatch on the
 * target engine's own waiting queue observed via the motor coordinator.
 * Create: 2026-09-22
 */

package gs

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"huawei.com/aigw/internal/base"
	"huawei.com/aigw/pkg/log"
)

// quotaAgeFallback bounds how long a perfectly static engine (no prefill,
// unchanged gauges) can hold the quota exhausted: one forced reset pe
// window keeps an idle-engine backlog draining. Matches the motor cache
// refresh cadence (~3s).
const quotaAgeFallback = 3 * time.Second

type engineSignal struct {
	running      int
	waiting      int
	kvUsage      float64
	queriesTotal float64 // monotonic prefill counter — snapshot version signal
}

// KvAdmissionController gates dispatch on engine-observed state. Signal mode
// polls the motor coordinator's metrics endpoint (~1s) and holds dispatch
// while the engine reports waiting>0; the engine's own scheduler is the
// authority on its capacity, so any KVC optimization is automatically
// accounted for. Frozen mode skips polling entirely and gates on a fixed
// per-instance in-flight cap (replay rounds). When signals are unavailable
// (stale beyond StaleAfterMs), the controller degrades to aigw-side counting
// against T = highWater if learned, else the configured batchSize.
type KvAdmissionController struct {
	mode         string
	metricsURL   string
	pollInterval time.Duration
	staleAfter   time.Duration
	burstCap     int
	hysteresis   int
	recoverCyc   int
	softCap      int
	batchSize    int

	// reqNum reports aigw's per-instance in-flight count.
	reqNum func(insUrl string) int

	mu             sync.Mutex
	signal         engineSignal
	multiInstance  bool // >1 instance_id observed: signals unmappable to aigw instances
	multiWarned    bool
	lastOk         time.Time
	healthyStreak  int
	pauseRemaining int // clear cycles left before resuming after waiting>0
	admits         map[string]int
	highWater      map[string]int

	degradedCount int64
	degradedSince time.Time
	degradedTotal time.Duration

	// kick is invoked after each poll cycle while the gate is open: the
	// poller drives dispatch retries itself instead of waiting for the
	// next arrival/completion event or the 10s survival ticker.
	kick func()

	// Quota is versioned: admits reset only when the observed snapshot
	// CONTENT changes (one motor refresh = at most burstCap admissions —
	// info rate equals action rate, no over-consumption of a stale view),
	// or after quotaAgeFallback on a perfectly static engine.
	quotaVersion string
	quotaResetAt time.Time
	// lastAdmitAt suppresses the age fallback while a dispatched request's
	// consequence is still in flight (unobserved): a recently-admitted
	// request makes a static snapshot EXPECTED, not a sign of a broken
	// observation pipeline.
	lastAdmitAt time.Time

	client *http.Client
}

func NewKvAdmissionController(cfg base.KvAdmissionConfig, batchSize int,
	reqNum func(insUrl string) int) *KvAdmissionController {
	return &KvAdmissionController{
		mode:         cfg.Mode,
		metricsURL:   cfg.MetricsURL,
		pollInterval: time.Duration(cfg.PollMs) * time.Millisecond,
		staleAfter:   time.Duration(cfg.StaleAfterMs) * time.Millisecond,
		burstCap:     cfg.BurstCapPerCycle,
		hysteresis:   cfg.HysteresisCycles,
		recoverCyc:   cfg.RecoverHealthyCycles,
		softCap:      cfg.SoftCap,
		batchSize:    batchSize,
		reqNum:       reqNum,
		admits:       map[string]int{},
		highWater:    map[string]int{},
		client:       &http.Client{Timeout: time.Second},
	}
}

// Start launches the signal poller (signal mode only; frozen needs no input).
func (c *KvAdmissionController) Start(ctx context.Context, wg *sync.WaitGroup) {
	if c == nil || c.mode != base.KvAdmissionSignal {
		return
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		ticker := time.NewTicker(c.pollInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				c.pollOnce(ctx)
				c.kickIfOpen()
			}
		}
	}()
}

// SetDispatchKick installs the post-poll dispatch trigger (signal mode).
func (c *KvAdmissionController) SetDispatchKick(f func()) {
	if c == nil {
		return
	}
	c.kick = f
}

// kickIfOpen triggers a dispatch attempt after a poll cycle when the gate
// is open. Called outside c.mu; the kick re-acquires the dispatch lock and
// its gate check re-takes c.mu — same order as the event-driven path, so
// no lock inversion. No kick while closed: avoids hold-log spam.
func (c *KvAdmissionController) kickIfOpen() {
	if c == nil || c.kick == nil {
		return
	}
	c.mu.Lock()
	open := c.mode == base.KvAdmissionSignal && c.healthyLocked() &&
		c.signal.waiting == 0 && c.pauseRemaining == 0
	c.mu.Unlock()
	if open {
		c.kick()
	}
}

func (c *KvAdmissionController) pollOnce(ctx context.Context) {
	ok := false
	var sigs map[string]engineSignal
	if req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.metricsURL, nil); err == nil {
		if resp, err := c.client.Do(req); err == nil {
			body, rerr := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
			resp.Body.Close()
			if rerr == nil && resp.StatusCode == http.StatusOK {
				sigs = parseEngineSignals(body)
				ok = len(sigs) > 0
			}
		}
	}

	c.mu.Lock()
	defer c.mu.Unlock()
	if !ok {
		// Failed poll grants no quota: resetting here would admit on
		// zero fresh information.
		c.healthyStreak = 0
		return
	}
	if len(sigs) > 1 {
		c.multiInstance = true
		if !c.multiWarned {
			c.multiWarned = true
			log.Warn().Msgf("[KvAdmission] multiple instance_ids in metrics; no aigw<->motor mapping yet, staying degraded")
		}
		return
	}
	for _, s := range sigs { // single engine: the sole entry applies to all instances
		c.signal = s
	}
	c.lastOk = time.Now()
	c.healthyStreak++
	if c.signal.waiting > 0 {
		// hysteresis clear cycles are denied before resuming; +1 covers
		// the cycle in which waiting was last seen positive.
		c.pauseRemaining = c.hysteresis + 1
	} else if c.pauseRemaining > 0 {
		c.pauseRemaining--
	}

	// Versioned quota: reset only when the snapshot content changed (a new
	// motor refresh) or on the age fallback for a fully static engine. A
	// poll that re-reads the cached refresh does NOT grant new quota, so
	// one snapshot is consumed at most once (per instance quota, K=1).
	version := fmt.Sprintf("%d/%d/%.4f/%.0f",
		c.signal.running, c.signal.waiting, c.signal.kvUsage, c.signal.queriesTotal)
	// The fallback additionally requires no recent admission: a dispatched
	// request whose consequence has not been observed yet makes a static
	// snapshot expected — the heartbeat quota must not fire mid-flight.
	// The admission gap uses staleAfter (not quotaAgeFallback): the
	// consequence-visible delay is cache phase (≤3s) + poll pickup (≤1s),
	// so an equal-length 3s guard still races the refresh cycle. Only a
	// snapshot static for the full staleness bound (8s) justifies a
	// heartbeat — by then the observation itself is suspect.
	fallbackDue := time.Since(c.quotaResetAt) >= quotaAgeFallback &&
		time.Since(c.lastAdmitAt) >= c.staleAfter
	if version != c.quotaVersion || fallbackDue {
		c.admits = map[string]int{}
		c.quotaVersion = version
		c.quotaResetAt = time.Now()
	}
}

// AllowDispatch is the ERR-layer admission gate for a dispatch to insUrl.
// The in-flight level is sampled on every call so highWater tracks the true
// operating point the signal mode ran at.
func (c *KvAdmissionController) AllowDispatch(insUrl string) bool {
	if c == nil {
		return true
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	inFlight := c.reqNum(insUrl)
	// The threshold must come from the operating point learned BEFORE this
	// call — sampling first would let the current level raise its own gate.
	prevHigh := c.highWater[insUrl]
	if inFlight > prevHigh {
		c.highWater[insUrl] = inFlight
	}

	if c.mode == base.KvAdmissionFrozen {
		if inFlight >= c.softCap {
			log.Debug().Msgf("[KvAdmission] frozen-cap: ins=%s inflight=%d softCap=%d", insUrl, inFlight, c.softCap)
			return false
		}
		return true
	}

	if !c.healthyLocked() {
		c.enterDegradedLocked()
		threshold := prevHigh
		if threshold <= 0 {
			threshold = c.batchSize
		}
		return inFlight < threshold
	}
	c.exitDegradedLocked()

	if c.signal.waiting > 0 || c.pauseRemaining > 0 {
		log.Debug().Msgf("[KvAdmission] hold: ins=%s waiting=%d pauseRem=%d sigAge=%s run=%d inflight=%d",
			insUrl, c.signal.waiting, c.pauseRemaining, time.Since(c.lastOk).Round(time.Millisecond), c.signal.running, inFlight)
		return false
	}
	if c.admits[insUrl] >= c.burstCap {
		log.Debug().Msgf("[KvAdmission] burst-cap: ins=%s admits=%d/%d inflight=%d", insUrl, c.admits[insUrl], c.burstCap, inFlight)
		return false
	}
	c.admits[insUrl]++
	c.lastAdmitAt = time.Now()
	return true
}

// LBCandidate is the LB-layer fast filter: skip instances whose engine
// reports waiting>0. It is a no-op unless signals are fresh, so degraded and
// frozen modes leave instance selection to the LB's own ordering.
func (c *KvAdmissionController) LBCandidate(insUrl string) bool {
	if c == nil {
		return true
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.mode != base.KvAdmissionSignal || !c.healthyLocked() {
		return true
	}
	return c.signal.waiting == 0
}

func (c *KvAdmissionController) healthyLocked() bool {
	return !c.multiInstance &&
		c.healthyStreak >= c.recoverCyc &&
		!c.lastOk.IsZero() &&
		time.Since(c.lastOk) < c.staleAfter
}

func (c *KvAdmissionController) enterDegradedLocked() {
	if c.degradedSince.IsZero() {
		c.degradedSince = time.Now()
		c.degradedCount++
		log.Warn().Msgf("[KvAdmission] signals unavailable — degraded to aigw-side in-flight gate (T=highWater or batchSize=%d)", c.batchSize)
	}
}

func (c *KvAdmissionController) exitDegradedLocked() {
	if !c.degradedSince.IsZero() {
		c.degradedTotal += time.Since(c.degradedSince)
		c.degradedSince = time.Time{}
		log.Warn().Msg("[KvAdmission] signals healthy again — back to waiting gate")
	}
}

// DegradationStats reports degraded episodes and cumulative degraded time.
func (c *KvAdmissionController) DegradationStats() (count int64, total time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	total = c.degradedTotal
	if !c.degradedSince.IsZero() {
		total += time.Since(c.degradedSince)
	}
	return c.degradedCount, total
}

// parseEngineSignals extracts running/waiting/kv-usage per instance_id from a
// Prometheus text exposition. Metric names are matched on the part after the
// last ':' so a renamed aggregator prefix still parses, while near-misses
// like num_requests_waiting_by_reason do not.
func parseEngineSignals(body []byte) map[string]engineSignal {
	sigs := map[string]engineSignal{}
	for _, line := range strings.Split(string(body), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		name, labels, value, ok := parsePromLine(line)
		if !ok {
			continue
		}
		short := name
		if i := strings.LastIndexByte(name, ':'); i >= 0 {
			short = name[i+1:]
		}
		s := sigs[labels["instance_id"]]
		switch short {
		case "num_requests_running":
			s.running = int(value)
		case "num_requests_waiting":
			s.waiting = int(value)
		case "kv_cache_usage_perc", "gpu_cache_usage_perc":
			s.kvUsage = value
		case "prefix_cache_queries_total":
			s.queriesTotal = value
		default:
			continue
		}
		sigs[labels["instance_id"]] = s
	}
	return sigs
}

func parsePromLine(line string) (name string, labels map[string]string, value float64, ok bool) {
	labels = map[string]string{}
	rest := ""
	if i := strings.IndexByte(line, '{'); i >= 0 {
		name = line[:i]
		j := strings.IndexByte(line[i:], '}')
		if j < 0 {
			return "", nil, 0, false
		}
		j += i
		for _, kv := range strings.Split(line[i+1:j], ",") {
			if eq := strings.IndexByte(kv, '='); eq > 0 {
				k := strings.TrimSpace(kv[:eq])
				v := strings.Trim(strings.TrimSpace(kv[eq+1:]), `"`)
				if k != "" {
					labels[k] = v
				}
			}
		}
		rest = strings.TrimSpace(line[j+1:])
	} else {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return "", nil, 0, false
		}
		name = fields[0]
		rest = fields[1]
	}
	if rest == "" {
		return "", nil, 0, false
	}
	v, err := strconv.ParseFloat(strings.Fields(rest)[0], 64)
	if err != nil || name == "" {
		return "", nil, 0, false
	}
	return name, labels, v, true
}
