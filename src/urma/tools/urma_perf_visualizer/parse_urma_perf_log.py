#!/usr/bin/env python3
"""
URMA Performance Log Visualizer

Parses URMA perf statistics from log output (including timestamps),
generates a JSON intermediate file, and creates an HTML visualization
using ECharts.js.

Handles real-world log format where:
  - Each log entry has a timestamp prefix (ISO 8601)
  - Multiple fields may share the same line (retry_count + wr_complete_cnt)
  - Latency table rows may be concatenated on the same line
  - Path statistics may appear inside SLOW LOG entries
  - Timestamps may not be in chronological order (data is sorted after parse)

Visualized data:
  - wr_complete_cnt     (per time window)
  - inflight_wr         (instantaneous, per path, up to 8 paths)
  - rtt_avg             (per time window, per path)
  - sqe_total           (per time window, per path)
  - pjetty_inflight_cnt (instantaneous)
  - Latency stats       (min/avg/max/p90/p99/p9999, grouped)

Usage:
    python parse_urma_perf_log.py <log_file> [-o output_dir]

Output:
    urma_perf_data.json  - Intermediate JSON data
    urma_perf_chart.html - ECharts visualization (open in browser)
"""

import re
import json
import argparse
import os
import sys
from collections import deque
from datetime import datetime

# ---------------------------------------------------------------------------
# Regex patterns
# ---------------------------------------------------------------------------

# Timestamp: 2026-08-21T22:16:40.932131
# Also handles space separator and optional fractional seconds.
RE_TIMESTAMP = re.compile(r'(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?)')

# Single-value fields (search anywhere in line)
RE_WR_COMPLETE = re.compile(r'wr_complete_cnt:\s*(\d+)')
RE_PJETTY_INFLIGHT = re.compile(r'pjetty_inflight_cnt:\s*(\d+)')

# Path-based fields: capture ONLY path entries, stop before next field name.
# This prevents rtt_avg from swallowing sqe_total values when they're on the
# same line.
_PATH_GROUP = r'((?:\[\d+,\d+\]:\d+\s*)+)'
RE_RTT_AVG = re.compile(r'rtt_avg:\s*' + _PATH_GROUP)
RE_SQE_TOTAL = re.compile(r'sqe_total:\s*' + _PATH_GROUP)
RE_INFLIGHT = re.compile(r'inflight_wr:\s*' + _PATH_GROUP)

# Individual path entry (used to parse the captured group)
RE_PATH_ENTRY = re.compile(r'\[(\d+),(\d+)\]:(\d+)')

# Latency table header: line containing "Type ... samples ... avg[ns]"
RE_LATENCY_HEADER = re.compile(r'Type.*samples.*avg\[ns\]')

# Latency data row: TYPE | n1 | n2 | n3 | n4 | n5 | n6 | n7 | n8
# Not anchored to line start — use finditer to catch multiple rows per line.
RE_LATENCY_ROW = re.compile(
    r'([A-Z][A-Z_]+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+'
    r'(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)'
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
CHART_COLORS = [
    '#5470c6', '#91cc75', '#fac858', '#ee6666',
    '#73c0de', '#3ba272', '#fc8452', '#9a60b4',
    '#ea7ccc', '#c4ccd5', '#d48265', '#749f83',
]

QUANTILES = ['min', 'avg', 'max', 'p90', 'p99', 'p9999']

# Maximum data points per chart. If a series exceeds this, it is downsampled
# in Python to keep the JSON small and the browser responsive.
MAX_POINTS = 10000


def _downsample(data_list, max_points=MAX_POINTS, key=None):
    """Downsample a sorted list to at most ~2*max_points entries, preserving
    extremes (peaks and valleys).

    Divides data into buckets of equal stride. For each bucket, keeps the
    data points with the minimum and maximum aggregate value (computed by
    the key function). If key is None, keeps the first element of each bucket.

    This ensures that extreme values (which matter most for performance
    analysis) are never lost, while still dramatically reducing data size.
    The result is sorted by timestamp.
    """
    n = len(data_list)
    if n <= max_points or key is None:
        return data_list
    stride = (n + max_points - 1) // max_points
    result = []
    for start in range(0, n, stride):
        bucket = data_list[start:start + stride]
        if not bucket:
            continue
        min_pt = min(bucket, key=key)
        max_pt = max(bucket, key=key)
        if min_pt is max_pt:
            result.append(min_pt)
        else:
            ts_key = lambda x: x[0] if x[0] is not None else float('inf')
            result.extend(sorted([min_pt, max_pt], key=ts_key))
    return result

LATENCY_GROUPS = [
    {'name': 'POST_SEND', 'types': ['UB_JETTY_POST_SEND', 'BOND_JETTY_POST_SEND']},
    {'name': 'POLL_JFC', 'types': ['UB_POLL_JFC', 'BOND_POLL_JFC']},
    {'name': 'BOND_SEND_RTT', 'types': ['BOND_SEND_RTT']},
    {'name': 'BOND_SQEIFLT_DIF', 'types': ['BOND_SQEIFLT_DIF']},
]

LATENCY_FIELDS = ['samples', 'avg', 'min', 'max', 'p90', 'p99', 'p9999']


def parse_timestamp(ts_str):
    """Parse ISO 8601 timestamp to Unix timestamp (float seconds).
    Handles both 'T' and space separators, optional fractional seconds."""
    # Normalize: replace space separator with 'T' for fromisoformat
    normalized = ts_str.replace(' ', 'T', 1)
    try:
        dt = datetime.fromisoformat(normalized)
    except (ValueError, AttributeError):
        try:
            dt = datetime.strptime(normalized, '%Y-%m-%dT%H:%M:%S.%f')
        except ValueError:
            dt = datetime.strptime(normalized, '%Y-%m-%dT%H:%M:%S')
    return dt.timestamp()


def _parse_path_data(captured):
    """Parse captured path group '[6,6]:12 [7,7]:12 ...' into a dict."""
    result = {}
    for m in RE_PATH_ENTRY.finditer(captured):
        li, ti, cnt = int(m.group(1)), int(m.group(2)), int(m.group(3))
        result['[%d,%d]' % (li, ti)] = cnt
    return result


def _find_ts_in_lines(lines):
    """Search lines (most recent first) for an ISO 8601 timestamp.
    Returns parsed Unix timestamp or None."""
    for line in reversed(lines):
        m = RE_TIMESTAMP.search(line)
        if m:
            return parse_timestamp(m.group(1))
    return None


def parse_log(log_path):
    """Parse URMA perf log with timestamp tracking.

    Returns a dict where each value is a list of (timestamp_or_None, data) pairs.
    Timestamps come from the log line prefix; data on continuation lines inherits
    the most recent timestamp.

    For SLOW LOG entries (inflight_wr, pjetty_inflight_cnt), the timestamp may
    be on a preceding line (the SLOW LOG header) rather than the current line.
    A ring buffer of recent lines is used to search backwards for the timestamp.
    """
    current_ts = None
    ts_count = 0
    first_ts_raw = None
    prev_lines = deque(maxlen=5)  # Ring buffer for backward timestamp search

    wr_complete = []       # [(ts, int)]
    inflight = []          # [(ts, {"[li,ti]": cnt})]
    rtt_avg = []           # [(ts, {"[li,ti]": ns})]
    sqe_total = []         # [(ts, {"[li,ti]": cnt})]
    pjetty_inflight = []   # [(ts, int)]
    latency = []           # [(ts, {type: {field: val}})]

    current_latency_snapshot = None

    with open(log_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            # --- Always check for timestamp first ---
            m = RE_TIMESTAMP.search(line)
            if m:
                current_ts = parse_timestamp(m.group(1))
                ts_count += 1
                if first_ts_raw is None:
                    first_ts_raw = m.group(0)

            # --- Latency table header (creates new snapshot) ---
            if RE_LATENCY_HEADER.search(line):
                current_latency_snapshot = {}
                latency.append((current_ts, current_latency_snapshot))

            # --- Latency data rows (finditer: multiple per line) ---
            for m in RE_LATENCY_ROW.finditer(line):
                if current_latency_snapshot is not None:
                    type_name = m.group(1)
                    current_latency_snapshot[type_name] = {
                        'samples': int(m.group(2)),
                        'avg': int(m.group(3)),
                        'min': int(m.group(4)),
                        'max': int(m.group(5)),
                        'p90': int(m.group(6)),
                        'p99': int(m.group(7)),
                        'p9999': int(m.group(8)),
                    }

            # --- Other fields (use 'if' not 'elif': may share a line) ---
            m = RE_WR_COMPLETE.search(line)
            if m:
                wr_complete.append((current_ts, int(m.group(1))))

            m = RE_RTT_AVG.search(line)
            if m:
                rtt_avg.append((current_ts, _parse_path_data(m.group(1))))

            m = RE_SQE_TOTAL.search(line)
            if m:
                sqe_total.append((current_ts, _parse_path_data(m.group(1))))

            # --- SLOW LOG fields: search prev_lines for timestamp ---
            # inflight_wr and pjetty_inflight_cnt appear in SLOW LOG entries
            # where the timestamp is on a preceding line (the SLOW LOG header).
            # If current_ts is None or doesn't match the SLOW LOG's timestamp,
            # search the ring buffer backwards for a more accurate timestamp.
            m = RE_INFLIGHT.search(line)
            if m:
                ts = current_ts
                if ts is None:
                    ts = _find_ts_in_lines(prev_lines)
                inflight.append((ts, _parse_path_data(m.group(1))))

            m = RE_PJETTY_INFLIGHT.search(line)
            if m:
                ts = current_ts
                if ts is None:
                    ts = _find_ts_in_lines(prev_lines)
                pjetty_inflight.append((ts, int(m.group(1))))

            # Update ring buffer at end of iteration
            prev_lines.append(line)

    return {
        'wr_complete': wr_complete,
        'inflight': inflight,
        'rtt_avg': rtt_avg,
        'sqe_total': sqe_total,
        'pjetty_inflight': pjetty_inflight,
        'latency': latency,
        '_diag': {
            'ts_count': ts_count,
            'first_ts_raw': first_ts_raw,
            'current_ts': current_ts,
        },
    }


def _find_base_ts(parsed):
    """Find the minimum timestamp across all data (for relative time axis)."""
    all_ts = []
    for key in ['wr_complete', 'pjetty_inflight', 'inflight', 'rtt_avg',
                'sqe_total', 'latency']:
        all_ts.extend(ts for ts, _ in parsed[key] if ts is not None)
    return min(all_ts) if all_ts else None


def _build_single(data_list, base_ts):
    """Sort (ts, value) pairs and build {time_axis, values}.
    Uses raw Unix timestamps when available; falls back to sequential indices."""
    has_ts = base_ts is not None
    if has_ts:
        data_list = sorted(data_list, key=lambda x: x[0] if x[0] is not None else float('inf'))
    data_list = _downsample(data_list, key=lambda x: x[1])

    time_axis = []
    values = []
    for i, (ts, val) in enumerate(data_list):
        if ts is not None and has_ts:
            time_axis.append(round(ts, 6))
        else:
            time_axis.append(float(i))
        values.append(val)
    return {'time_axis': time_axis, 'values': values}


def _build_path(data_list, base_ts):
    """Sort (ts, {path: val}) pairs and build {time_axis, paths, values}."""
    has_ts = base_ts is not None
    if has_ts:
        data_list = sorted(data_list, key=lambda x: x[0] if x[0] is not None else float('inf'))

    # Collect all_paths from full (pre-downsample) data
    all_paths = set()
    for ts, snap in data_list:
        all_paths.update(snap.keys())
    all_paths = sorted(all_paths)

    data_list = _downsample(data_list, key=lambda x: sum(x[1].values()))

    time_axis = []
    snapshots = []
    for i, (ts, snap) in enumerate(data_list):
        if ts is not None and has_ts:
            time_axis.append(round(ts, 6))
        else:
            time_axis.append(float(i))
        snapshots.append(snap)

    return {
        'time_axis': time_axis,
        'paths': all_paths,
        'values': {
            path: [snap.get(path, 0) for snap in snapshots]
            for path in all_paths
        },
    }


def _port_label(local_idx):
    """Convert local_idx to a descriptive die/port label.

    Mapping: 6->die0-port4, 7->die0-port5, 15->die1-port4, 16->die1-port5.
    die = 1 if local_idx >= 8, else 0.
    port = 4 + ((local_idx + die) & 1).
    """
    die = 1 if local_idx >= 8 else 0
    port = 4 + ((local_idx + die) & 1)
    return 'die%d-port%d' % (die, port)


def _build_port(data_list, base_ts, mode):
    """Aggregate path data by local_idx into port-level time series.

    Each path key is "[local_idx,target_idx]". Paths sharing the same
    local_idx belong to the same port.

    mode: 'sum' for cnt-type metrics (inflight_wr, sqe_total),
          'avg' for rtt-type metrics (rtt_avg).
    """
    has_ts = base_ts is not None
    if has_ts:
        data_list = sorted(data_list, key=lambda x: x[0] if x[0] is not None else float('inf'))

    # Collect all_ports from full (pre-downsample) data
    all_ports = set()
    for ts, snap in data_list:
        for path in snap.keys():
            li = int(path.strip('[]').split(',')[0])
            all_ports.add(li)
    all_ports = sorted(all_ports)

    data_list = _downsample(data_list, key=lambda x: sum(x[1].values()))

    time_axis = []
    snapshots = []
    for i, (ts, snap) in enumerate(data_list):
        if ts is not None and has_ts:
            time_axis.append(round(ts, 6))
        else:
            time_axis.append(float(i))
        snapshots.append(snap)

    port_names = [_port_label(p) for p in all_ports]

    port_values = {name: [] for name in port_names}
    for snap in snapshots:
        port_sum = {}
        port_count = {}
        for path, val in snap.items():
            li = int(path.strip('[]').split(',')[0])
            port_sum[li] = port_sum.get(li, 0) + val
            port_count[li] = port_count.get(li, 0) + 1
        for port in all_ports:
            pname = _port_label(port)
            if port in port_sum:
                if mode == 'sum':
                    port_values[pname].append(port_sum[port])
                else:
                    port_values[pname].append(int(round(port_sum[port] / port_count[port])))
            else:
                port_values[pname].append(0)

    return {
        'time_axis': time_axis,
        'paths': port_names,
        'values': port_values,
    }


def _build_latency(data_list, base_ts):
    """Sort (ts, {type: {field: val}}) pairs and build latency data."""
    has_ts = base_ts is not None
    # Filter out empty snapshots
    filtered = [(ts, snap) for ts, snap in data_list if snap]
    if has_ts:
        filtered = sorted(filtered, key=lambda x: x[0] if x[0] is not None else float('inf'))

    # Collect all_types from the FULL (pre-downsample) data so that types
    # which only appear in some snapshots are not lost during downsampling.
    all_types = set()
    for ts, snap in filtered:
        all_types.update(snap.keys())
    all_types = sorted(all_types)

    def _latency_max(pt):
        snap = pt[1]
        # Use only actual latency fields, exclude 'samples' (a count, not latency)
        maxes = [t.get('max', 0) for t in snap.values()]
        return max(maxes) if maxes else 0

    filtered = _downsample(filtered, key=_latency_max)

    time_axis = []
    snapshots = []
    for i, (ts, snap) in enumerate(filtered):
        if ts is not None and has_ts:
            time_axis.append(round(ts, 6))
        else:
            time_axis.append(float(i))
        snapshots.append(snap)

    types_data = {}
    for t in all_types:
        types_data[t] = {f: [] for f in LATENCY_FIELDS}
        for snap in snapshots:
            entry = snap.get(t)
            if entry:
                for f in LATENCY_FIELDS:
                    types_data[t][f].append(entry[f])
            else:
                for f in LATENCY_FIELDS:
                    types_data[t][f].append(0)

    return {'time_axis': time_axis, 'types': types_data}


def build_json(parsed):
    """Build the complete JSON data structure."""
    base_ts = _find_base_ts(parsed)
    has_ts = base_ts is not None
    data = {
        'has_timestamps': has_ts,
        'wr_complete_cnt': _build_single(parsed['wr_complete'], base_ts),
        'inflight_wr': _build_path(parsed['inflight'], base_ts),
        'inflight_wr_port': _build_port(parsed['inflight'], base_ts, 'sum'),
        'rtt_avg': _build_path(parsed['rtt_avg'], base_ts),
        'rtt_avg_port': _build_port(parsed['rtt_avg'], base_ts, 'avg'),
        'sqe_total': _build_path(parsed['sqe_total'], base_ts),
        'sqe_total_port': _build_port(parsed['sqe_total'], base_ts, 'sum'),
        'pjetty_inflight_cnt': _build_single(parsed['pjetty_inflight'], base_ts),
        'latency_stats': _build_latency(parsed['latency'], base_ts),
        'latency_groups': LATENCY_GROUPS,
    }
    return data


# ---------------------------------------------------------------------------
# HTML template
# ---------------------------------------------------------------------------
HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>URMA Performance Visualization</title>
    <script src="https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js"></script>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Arial, sans-serif;
            background: #f0f2f5;
            padding: 24px;
        }
        h1 { color: #1d1d1d; margin-bottom: 24px; font-size: 24px; }
        h2 { color: #333; font-size: 18px; margin-bottom: 12px; }
        .chart-card {
            background: #fff;
            border-radius: 12px;
            padding: 24px;
            margin-bottom: 24px;
            box-shadow: 0 2px 12px rgba(0,0,0,0.08);
        }
        .chart { width: 100%; height: 450px; }
        .summary {
            display: flex; gap: 24px; margin-top: 12px;
            font-size: 13px; color: #666;
        }
        .summary span { margin-right: 4px; }
        .summary b { color: #333; }
        .quantile-buttons {
            display: flex; gap: 4px; margin-bottom: 12px;
        }
        .qbtn {
            padding: 4px 14px;
            border: 1px solid #d0d0d0;
            border-radius: 4px;
            background: #fff;
            cursor: pointer;
            font-size: 13px;
            color: #555;
            transition: all 0.2s;
        }
        .qbtn:hover { background: #e8e8e8; }
        .qbtn.active {
            background: #5470c6;
            color: #fff;
            border-color: #5470c6;
        }
        .section-title {
            color: #999;
            font-size: 14px;
            margin: 24px 0 12px;
            padding-bottom: 8px;
            border-bottom: 1px solid #e0e0e0;
        }
    </style>
</head>
<body>
    <h1>URMA Performance Visualization</h1>

    <div class="section-title">Path / Port Statistics (toggle view)</div>
    <div class="chart-card">
        <h2>In-Flight WR Count</h2>
        <div class="quantile-buttons" id="btns_inflight">
            <button class="qbtn active" data-view="path" onclick="switchView('inflight', 'path')">Per Path</button>
            <button class="qbtn" data-view="port" onclick="switchView('inflight', 'port')">Per Port</button>
        </div>
        <div id="chart_inflight" class="chart"></div>
    </div>
    <div class="chart-card">
        <h2>RTT Average [ns]</h2>
        <div class="quantile-buttons" id="btns_rtt">
            <button class="qbtn active" data-view="path" onclick="switchView('rtt', 'path')">Per Path</button>
            <button class="qbtn" data-view="port" onclick="switchView('rtt', 'port')">Per Port</button>
        </div>
        <div id="chart_rtt" class="chart"></div>
    </div>
    <div class="chart-card">
        <h2>SQE Total</h2>
        <div class="quantile-buttons" id="btns_sqe">
            <button class="qbtn active" data-view="path" onclick="switchView('sqe', 'path')">Per Path</button>
            <button class="qbtn" data-view="port" onclick="switchView('sqe', 'port')">Per Port</button>
        </div>
        <div id="chart_sqe" class="chart"></div>
    </div>

    <div class="section-title">Counters</div>
    <div class="chart-card">
        <h2>WR Complete Count (per window)</h2>
        <div id="chart_wr_complete" class="chart"></div>
        <div class="summary" id="summary_wr_complete"></div>
    </div>
    <div class="chart-card">
        <h2>PJetty In-Flight Count (instantaneous)</h2>
        <div id="chart_pjetty" class="chart"></div>
        <div class="summary" id="summary_pjetty"></div>
    </div>

    <div class="section-title">Latency Statistics (toggle quantile)</div>
    <div id="latency_charts"></div>

    <script>
    var DATA = __DATA__;
    var COLORS = __COLORS__;
    var QUANTILES = __QUANTILES__;
    var allCharts = [];

    var HAS_TS = DATA.has_timestamps;

    // Pass through raw Unix timestamp (seconds, float with µs precision) or
    // sequential index. ECharts value axis preserves full numeric precision.
    function toX(t) {
        return t;
    }

    function fmtDateTime(val, sep) {
        sep = (typeof sep === 'string') ? sep : '\n';
        var wholeSec = Math.floor(val);
        var micro = Math.round((val - wholeSec) * 1000000);
        if (micro >= 1000000) { micro -= 1000000; wholeSec += 1; }
        var d = new Date(wholeSec * 1000);
        function pad2(n) { return n < 10 ? '0' + n : '' + n; }
        function pad6(n) { var s = '' + n; while (s.length < 6) s = '0' + s; return s; }
        return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) +
               sep + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds()) +
               '.' + pad6(micro);
    }

    function fmtRelTime(val) {
        return (Math.round(val * 10) / 10) + 's';
    }

    function makeSummary(values) {
        if (!values || !values.length) return 'No data';
        var min = Math.min.apply(null, values);
        var max = Math.max.apply(null, values);
        var sum = values.reduce(function(a, b) { return a + b; }, 0);
        var avg = (sum / values.length).toFixed(1);
        return '<span>samples: <b>' + values.length + '</b></span>' +
               '<span>min: <b>' + min + '</b></span>' +
               '<span>max: <b>' + max + '</b></span>' +
               '<span>avg: <b>' + avg + '</b></span>' +
               '<span>total: <b>' + sum + '</b></span>';
    }

    var xAxisCfg = HAS_TS ? {
        type: 'value',
        name: 'Time',
        nameLocation: 'middle',
        nameGap: 55,
        axisLabel: { formatter: fmtDateTime, rotate: 30, hideOverlap: true },
        minInterval: 0.000001,
        min: 'dataMin',
        max: 'dataMax',
    } : {
        type: 'value',
        name: 'Index',
        nameLocation: 'middle',
        nameGap: 35,
        axisLabel: { formatter: fmtRelTime },
    };

    // Shared tooltip: shows date-time header when timestamps are available.
    var tooltipCfg = {
        trigger: 'axis',
        formatter: function(params) {
            if (!params || !params.length) return '';
            var header;
            if (HAS_TS) {
                header = fmtDateTime(params[0].axisValue, '<br/>');
            } else {
                header = 'Index: ' + fmtRelTime(params[0].axisValue);
            }
            var lines = [header];
            params.forEach(function(p) {
                lines.push(p.marker + p.seriesName + ': ' + p.value[1]);
            });
            return lines.join('<br/>');
        },
    };

    // -- Per-path / per-port chart helper --
    var pathChartStates = {};

    function createPathChart(containerId, id, field, portField, yLabel) {
        var chart = echarts.init(document.getElementById(containerId));
        var state = { view: 'path', chart: chart, field: field, portField: portField, yLabel: yLabel };

        state.update = function() {
            var currentData = state.view === 'path' ? DATA[state.field] : DATA[state.portField];
            var series = currentData.paths.map(function(path, i) {
                var data = currentData.time_axis.map(function(t, j) {
                    return [toX(t), currentData.values[path][j]];
                });
                return {
                    name: path,
                    type: 'line',
                    data: data,
                    symbol: 'none',
                    lineStyle: { width: 2 },
                    itemStyle: { color: COLORS[i % COLORS.length] },
                };
            });
            chart.setOption({
                tooltip: tooltipCfg,
                legend: { data: currentData.paths, top: 0, type: 'scroll' },
                grid: { left: 80, right: 30, top: 50, bottom: 90 },
                xAxis: xAxisCfg,
                yAxis: { type: 'value', name: state.yLabel },
                dataZoom: [{ type: 'inside' }, { type: 'slider', height: 20, bottom: 8 }],
                series: series,
            }, true);
        };

        state.update();
        allCharts.push(chart);
        pathChartStates[id] = state;
        return state;
    }

    function switchView(id, view) {
        var state = pathChartStates[id];
        if (!state) return;
        state.view = view;
        state.update();
        var btns = document.querySelectorAll('#btns_' + id + ' .qbtn');
        for (var i = 0; i < btns.length; i++) {
            btns[i].classList.toggle('active', btns[i].getAttribute('data-view') === view);
        }
    }

    // -- Single-value chart helper --
    function createSingleChart(containerId, field, yLabel, color) {
        var d = DATA[field];
        var chart = echarts.init(document.getElementById(containerId));
        var data = d.time_axis.map(function(t, j) {
            return [toX(t), d.values[j]];
        });
        chart.setOption({
            tooltip: tooltipCfg,
            grid: { left: 80, right: 30, top: 30, bottom: 90 },
            xAxis: xAxisCfg,
            yAxis: { type: 'value', name: yLabel },
            dataZoom: [{ type: 'inside' }, { type: 'slider', height: 20, bottom: 8 }],
            series: [{
                name: field,
                type: 'line',
                data: data,
                symbol: 'none',
                lineStyle: { width: 2 },
                itemStyle: { color: color },
                areaStyle: { opacity: 0.08 },
            }],
        });
        allCharts.push(chart);
        return chart;
    }

    // -- Latency chart helpers --
    var latencyChartStates = {};

    function initLatencyChart(domId, groupIdx) {
        var group = DATA.latency_groups[groupIdx];
        var chart = echarts.init(document.getElementById(domId));
        var state = { quantile: 'avg', chart: chart, group: group };

        state.update = function() {
            var series = group.types.map(function(type, i) {
                var td = DATA.latency_stats.types[type];
                if (!td) return null;
                var data = DATA.latency_stats.time_axis.map(function(t, j) {
                    return [toX(t), td[state.quantile][j]];
                });
                return {
                    name: type,
                    type: 'line',
                    data: data,
                    symbol: 'none',
                    lineStyle: { width: 2 },
                    itemStyle: { color: COLORS[i % COLORS.length] },
                };
            }).filter(Boolean);

            var visibleTypes = group.types.filter(function(t) {
                return !!DATA.latency_stats.types[t];
            });

            chart.setOption({
                tooltip: tooltipCfg,
                legend: { data: visibleTypes, top: 0, type: 'scroll' },
                grid: { left: 80, right: 30, top: 40, bottom: 90 },
                xAxis: xAxisCfg,
                yAxis: { type: 'value', name: state.quantile + ' [ns]' },
                dataZoom: [{ type: 'inside' }, { type: 'slider', height: 20, bottom: 8 }],
                series: series,
            }, true);
        };

        state.update();
        allCharts.push(chart);
        return state;
    }

    function switchQuantile(id, quantile) {
        var state = latencyChartStates[id];
        if (!state) return;
        state.quantile = quantile;
        state.update();
        var btns = document.querySelectorAll('#btns_' + id + ' .qbtn');
        for (var i = 0; i < btns.length; i++) {
            btns[i].classList.toggle('active', btns[i].getAttribute('data-q') === quantile);
        }
    }

    // -- Initialize all charts --
    createPathChart('chart_inflight', 'inflight', 'inflight_wr', 'inflight_wr_port', 'In-Flight WRs');
    createPathChart('chart_rtt', 'rtt', 'rtt_avg', 'rtt_avg_port', 'RTT [ns]');
    createPathChart('chart_sqe', 'sqe', 'sqe_total', 'sqe_total_port', 'SQE Total');

    createSingleChart('chart_wr_complete', 'wr_complete_cnt', 'Count', '#5470c6');
    createSingleChart('chart_pjetty', 'pjetty_inflight_cnt', 'Count', '#ee6666');

    document.getElementById('summary_wr_complete').innerHTML =
        makeSummary(DATA.wr_complete_cnt.values);
    document.getElementById('summary_pjetty').innerHTML =
        makeSummary(DATA.pjetty_inflight_cnt.values);

    // Latency groups
    var latContainer = document.getElementById('latency_charts');
    DATA.latency_groups.forEach(function(group, gi) {
        var id = 'lat_' + gi;
        var card = document.createElement('div');
        card.className = 'chart-card';
        card.innerHTML =
            '<h2>' + group.name + ' Latency</h2>' +
            '<div class="quantile-buttons" id="btns_' + id + '">' +
                QUANTILES.map(function(q) {
                    return '<button class="qbtn' + (q === 'avg' ? ' active' : '') +
                           '" data-q="' + q + '" ' +
                           'onclick="switchQuantile(\'' + id + '\', \'' + q + '\')">' +
                           q + '</button>';
                }).join('') +
            '</div>' +
            '<div id="chart_' + id + '" class="chart"></div>';
        latContainer.appendChild(card);
        latencyChartStates[id] = initLatencyChart('chart_' + id, gi);
    });

    // Responsive
    window.addEventListener('resize', function() {
        for (var i = 0; i < allCharts.length; i++) {
            allCharts[i].resize();
        }
    });
    </script>
</body>
</html>
"""


def generate_html(data):
    """Generate HTML by injecting JSON into the template."""
    html = HTML_TEMPLATE
    html = html.replace('__DATA__', json.dumps(data, indent=2))
    html = html.replace('__COLORS__', json.dumps(CHART_COLORS))
    html = html.replace('__QUANTILES__', json.dumps(QUANTILES))
    return html


def main():
    parser = argparse.ArgumentParser(
        description='URMA Performance Log Visualizer')
    parser.add_argument('log_file', help='Path to the log file')
    parser.add_argument('-o', '--output', default='.',
                        help='Output directory (default: current dir)')
    parser.add_argument('--debug', action='store_true',
                        help='Print debug info (first lines, timestamp matches)')
    args = parser.parse_args()

    if not os.path.isfile(args.log_file):
        print('Error: log file not found: %s' % args.log_file, file=sys.stderr)
        sys.exit(1)

    if args.debug:
        print('=== DEBUG: First 10 lines with timestamp detection ===')
        with open(args.log_file, 'r', encoding='utf-8', errors='ignore') as f:
            for i, line in enumerate(f):
                if i >= 10:
                    break
                line_stripped = line.rstrip('\n\r')
                ts_match = RE_TIMESTAMP.search(line)
                ts_info = 'TS=%s' % ts_match.group(0) if ts_match else 'NO_TS'
                print('Line %d [%s]: %.100s' % (i, ts_info, line_stripped))
        print()

    os.makedirs(args.output, exist_ok=True)

    parsed = parse_log(args.log_file)

    print('Parsed %d wr_complete_cnt samples' % len(parsed['wr_complete']))
    print('Parsed %d inflight_wr snapshots' % len(parsed['inflight']))
    print('Parsed %d rtt_avg snapshots' % len(parsed['rtt_avg']))
    print('Parsed %d sqe_total snapshots' % len(parsed['sqe_total']))
    print('Parsed %d pjetty_inflight_cnt samples' % len(parsed['pjetty_inflight']))
    print('Parsed %d latency table snapshots' % len(parsed['latency']))

    # --- Timestamp diagnostics ---
    diag = parsed.get('_diag', {})
    ts_count = diag.get('ts_count', 0)
    first_ts_raw = diag.get('first_ts_raw', None)
    base_ts = _find_base_ts(parsed)
    if ts_count > 0:
        print('Timestamps: %d found, first=%s, base_ts=%s' % (ts_count, first_ts_raw, base_ts))
        if base_ts is None:
            print('WARNING: timestamps found in log but no data points have '
                  'timestamps — data may appear before first timestamp line.',
                  file=sys.stderr)
    else:
        print('WARNING: no timestamps found in log — charts will use sequential '
              'indices (0, 1, 2, ...) instead of real time. '
              'Check that log lines contain ISO 8601 timestamps like '
              '"2026-08-21T22:16:40.932131".', file=sys.stderr)

    # --- Per-data-type timestamp check (debug) ---
    if args.debug:
        for key in ['wr_complete', 'inflight', 'rtt_avg', 'sqe_total',
                     'pjetty_inflight', 'latency']:
            lst = parsed[key]
            if lst:
                first_ts = lst[0][0]
                last_ts = lst[-1][0]
                print('  %-20s: %d entries, first_ts=%s, last_ts=%s' % (
                    key, len(lst), first_ts, last_ts))

    # Remove diagnostic data before build_json
    parsed.pop('_diag', None)

    if not any(parsed.values()):
        print('Warning: no data found in log file', file=sys.stderr)
        sys.exit(1)

    data = build_json(parsed)

    json_path = os.path.join(args.output, 'urma_perf_data.json')
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2)
    print('JSON  -> %s' % json_path)

    html = generate_html(data)
    html_path = os.path.join(args.output, 'urma_perf_chart.html')
    with open(html_path, 'w', encoding='utf-8') as f:
        f.write(html)
    print('HTML  -> %s' % html_path)


if __name__ == '__main__':
    main()
