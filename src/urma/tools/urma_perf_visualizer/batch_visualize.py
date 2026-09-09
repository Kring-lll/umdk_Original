#!/usr/bin/env python3
"""
URMA Batch Visualizer

Recursively searches for urma_perf.log files under patha, generates
HTML+JSON charts for each using parse_urma_perf_log, and creates a
summary HTML at patha's parent directory with a dropdown to switch
between different subdirectories' charts.

Usage:
    python batch_visualize.py <patha>

Arguments:
    patha - root directory containing filtered urma_perf.log files
"""

import argparse
import concurrent.futures
import os
import sys
import json

# Add this script's directory to path so we can import parse_urma_perf_log
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

from parse_urma_perf_log import parse_log, build_json, generate_html

LOG_FILENAME = 'urma_perf.log'
JSON_FILENAME = 'urma_perf_data.json'
HTML_FILENAME = 'urma_perf_chart.html'
SUMMARY_HTML = 'urma_perf_summary.html'

# Parallel workers for log processing. Parsing/JSON/HTML generation is pure
# CPU-bound Python, which the GIL serializes inside threads, so a process
# pool is used for real parallelism. Slightly oversubscribed (2x cores) so
# file reads of the next log overlap with parsing of the current one.
MAX_WORKERS = min(32, (os.cpu_count() or 4) * 2)


def find_log_files(patha):
    """Recursively find all urma_perf.log files under patha."""
    matches = []
    for root, dirs, files in os.walk(patha):
        if LOG_FILENAME in files:
            matches.append(os.path.join(root, LOG_FILENAME))
    return matches


def process_log(log_path):
    """Parse a log file and generate JSON+HTML in the same directory.

    Runs inside a worker process: prints nothing and returns
    (success, rtt_values, messages) so the main process can print the
    messages without interleaving. rtt_values is a list of ints from
    BOND_SEND_RTT max/p9999/p99 fields.
    """
    msgs = []
    log_dir = os.path.dirname(log_path)
    parsed = parse_log(log_path)

    msgs.append('  Parsed %d wr_complete, %d inflight, %d rtt_avg, %d sqe_total, '
                '%d pjetty_inflight, %d latency snapshots'
                % (len(parsed['wr_complete']), len(parsed['inflight']),
                   len(parsed['rtt_avg']), len(parsed['sqe_total']),
                   len(parsed['pjetty_inflight']), len(parsed['latency'])))

    # Extract BOND_SEND_RTT values (max, p9999, p99) for top-200 distribution
    rtt_values = []
    for ts, snap in parsed['latency']:
        rtt_entry = snap.get('BOND_SEND_RTT')
        if rtt_entry:
            for field in ('max', 'p9999', 'p99'):
                val = rtt_entry.get(field, 0)
                if val > 0:
                    rtt_values.append(val)

    parsed.pop('_diag', None)

    if not any(parsed.values()):
        msgs.append('  Warning: no data found in %s, skipping' % log_path)
        return False, rtt_values, msgs

    data = build_json(parsed)

    json_path = os.path.join(log_dir, JSON_FILENAME)
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2)

    html = generate_html(data)
    html_path = os.path.join(log_dir, HTML_FILENAME)
    with open(html_path, 'w', encoding='utf-8') as f:
        f.write(html)

    msgs.append('  JSON -> %s' % json_path)
    msgs.append('  HTML -> %s' % html_path)
    msgs.append('  BOND_SEND_RTT samples for top-200: %d' % len(rtt_values))
    return True, rtt_values, msgs


def generate_summary_html(patha_abs, log_paths, rtt_data):
    """Generate summary HTML with two-level cascading dropdown and top-200 distribution.

    First level: case name (first directory component under patha)
    Second level: sub-path relative to the case directory

    The summary HTML is placed inside patha directory.

    rtt_data: list of (value, case_name, sub_display, full_display) tuples
    """
    # Group log files by case (first directory component under patha)
    cases = {}  # {case_name: [(sub_display, html_rel), ...]}
    for log_path in log_paths:
        log_dir = os.path.dirname(log_path)
        rel_to_patha = os.path.relpath(log_dir, patha_abs)
        parts = rel_to_patha.split(os.sep)
        case_name = parts[0]
        sub_path = os.sep.join(parts[1:]) if len(parts) > 1 else ''

        # Summary is inside patha, so relative path is just rel_to_patha
        html_rel = os.path.join(rel_to_patha, HTML_FILENAME)
        html_rel = html_rel.replace(os.sep, '/')
        sub_display = sub_path.replace(os.sep, '/') if sub_path else '(root)'

        cases.setdefault(case_name, []).append((sub_display, html_rel))

    case_names = sorted(cases.keys())
    for cn in case_names:
        cases[cn].sort()

    summary_path = os.path.join(patha_abs, SUMMARY_HTML)

    # Build option elements for case dropdown
    case_options = '\n'.join(
        '        <option value="%s">%s</option>' % (cn, cn)
        for cn in case_names
    )

    # Build JSON data for second-level population
    cases_data = {cn: [{'sub': s, 'path': p} for s, p in cases[cn]]
                  for cn in case_names}

    # --- Compute global top-200 distribution ---
    # For each directory appearing in the top-200, keep its sample count and
    # the max latency among its top-200 samples; sort by max latency desc.
    global_top200 = sorted(rtt_data, key=lambda x: x[0], reverse=True)[:200]
    global_dist = {}
    for val, case, sub, full in global_top200:
        entry = global_dist.setdefault(full, {'count': 0, 'max': 0})
        entry['count'] += 1
        if val > entry['max']:
            entry['max'] = val
    global_dist_sorted = sorted(global_dist.items(), key=lambda x: -x[1]['max'])

    # --- Compute per-case top-200 distribution ---
    case_values = {}
    for val, case, sub, full in rtt_data:
        case_values.setdefault(case, []).append((val, sub))
    per_case_dist = {}
    for case_name, vals in case_values.items():
        top200 = sorted(vals, key=lambda x: x[0], reverse=True)[:200]
        dist = {}
        for val, sub in top200:
            entry = dist.setdefault(sub, {'count': 0, 'max': 0})
            entry['count'] += 1
            if val > entry['max']:
                entry['max'] = val
        per_case_dist[case_name] = sorted(dist.items(), key=lambda x: -x[1]['max'])

    dist_data = {
        'global': [[name, e['count'], e['max']] for name, e in global_dist_sorted],
        'per_case': {cn: [[name, e['count'], e['max']] for name, e in dist]
                     for cn, dist in per_case_dist.items()},
    }

    html_content = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>URMA Performance Summary</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Arial, sans-serif;
            background: #f0f2f5;
            padding: 16px;
        }
        .header {
            background: #fff;
            border-radius: 12px;
            padding: 20px 24px;
            margin-bottom: 16px;
            box-shadow: 0 2px 12px rgba(0,0,0,0.08);
            display: flex;
            align-items: center;
            gap: 16px;
            flex-wrap: wrap;
        }
        h1 { color: #1d1d1d; font-size: 22px; white-space: nowrap; }
        .label { color: #666; font-size: 14px; white-space: nowrap; }
        select {
            padding: 8px 12px;
            font-size: 15px;
            border: 2px solid #5470c6;
            border-radius: 8px;
            background: #fff;
            cursor: pointer;
            min-width: 200px;
        }
        .view-toggle {
            display: flex;
            gap: 0;
            margin-left: auto;
        }
        .view-toggle button {
            padding: 8px 20px;
            font-size: 14px;
            border: 2px solid #5470c6;
            background: #fff;
            color: #5470c6;
            cursor: pointer;
        }
        .view-toggle button:first-child { border-radius: 8px 0 0 8px; }
        .view-toggle button:last-child { border-radius: 0 8px 8px 0; border-left: none; }
        .view-toggle button.active { background: #5470c6; color: #fff; }
        iframe {
            width: 100%;
            height: calc(100vh - 100px);
            border: none;
            border-radius: 12px;
            box-shadow: 0 2px 12px rgba(0,0,0,0.08);
            background: #fff;
        }
        #chartsView.hidden { display: none; }
        .dist-view { display: none; }
        .dist-view.active { display: block; }
        .dist-section {
            background: #fff;
            border-radius: 12px;
            padding: 24px;
            margin-bottom: 16px;
            box-shadow: 0 2px 12px rgba(0,0,0,0.08);
        }
        .dist-section h2 { color: #1d1d1d; font-size: 18px; margin-bottom: 12px; }
        .dist-section h3 { color: #333; font-size: 15px; margin: 16px 0 8px; }
        .toggle-header {
            cursor: pointer;
            user-select: none;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .toggle-header::before {
            content: '▶';
            font-size: 12px;
            color: #5470c6;
            transition: transform 0.2s;
        }
        .toggle-header.open::before { transform: rotate(90deg); }
        .toggle-content { display: none; }
        .toggle-content.open { display: block; }
        table { border-collapse: collapse; width: 100%; }
        th, td { border: 1px solid #e0e0e0; padding: 6px 12px; text-align: left; }
        th { background: #f5f5f5; font-weight: 600; }
        td.count { text-align: right; font-variant-numeric: tabular-nums; }
        tr:nth-child(even) td { background: #fafafa; }
    </style>
</head>
<body>
    <div class="header">
        <h1>URMA Perf Summary</h1>
        <span class="label">Case:</span>
        <select id="caseSelector" onchange="updateSubPaths()">
__CASE_OPTIONS__
        </select>
        <span class="label">Sub-path:</span>
        <select id="subSelector" onchange="switchChart()">
        </select>
        <div class="view-toggle">
            <button id="btnCharts" class="active" onclick="switchView('charts')">Charts</button>
            <button id="btnDist" onclick="switchView('dist')">Top-200 Distribution</button>
        </div>
    </div>
    <div id="chartsView">
        <iframe id="chartFrame" src=""></iframe>
    </div>
    <div id="distView" class="dist-view">
        <div class="dist-section">
            <h2 class="toggle-header" onclick="toggleSection(this)">Global Top-200 BOND_SEND_RTT (pmax / p9999 / p99, sorted by max)</h2>
            <div class="toggle-content">
                <table id="globalDistTable">
                    <thead><tr><th>Directory</th><th>Count</th><th>Max RTT [ns]</th></tr></thead>
                    <tbody></tbody>
                </table>
            </div>
        </div>
        <div class="dist-section">
            <h2 class="toggle-header" onclick="toggleSection(this)">Per-Case Top-200 (sorted by max)</h2>
            <div class="toggle-content" id="perCaseDist"></div>
        </div>
    </div>
    <script>
        var casesData = __CASES_JSON__;
        var distData = __DIST_JSON__;

        function updateSubPaths() {
            var caseSel = document.getElementById('caseSelector');
            var subSel = document.getElementById('subSelector');
            var caseName = caseSel.value;
            subSel.innerHTML = '';
            var entries = casesData[caseName] || [];
            for (var i = 0; i < entries.length; i++) {
                var opt = document.createElement('option');
                opt.value = entries[i].path;
                opt.textContent = entries[i].sub;
                subSel.appendChild(opt);
            }
            if (entries.length > 0) {
                switchChart();
            }
        }

        function switchChart() {
            var subSel = document.getElementById('subSelector');
            var frame = document.getElementById('chartFrame');
            frame.src = subSel.value;
        }

        function switchView(view) {
            var chartsView = document.getElementById('chartsView');
            var distView = document.getElementById('distView');
            var btnCharts = document.getElementById('btnCharts');
            var btnDist = document.getElementById('btnDist');
            if (view === 'charts') {
                chartsView.classList.remove('hidden');
                distView.classList.remove('active');
                btnCharts.classList.add('active');
                btnDist.classList.remove('active');
            } else {
                chartsView.classList.add('hidden');
                distView.classList.add('active');
                btnCharts.classList.remove('active');
                btnDist.classList.add('active');
                renderDist();
            }
        }

        function toggleSection(header) {
            var content = header.nextElementSibling;
            header.classList.toggle('open');
            content.classList.toggle('open');
        }

        function renderDist() {
            // Global distribution table (sorted by max latency desc)
            var tbody = document.querySelector('#globalDistTable tbody');
            tbody.innerHTML = '';
            var globalData = distData.global || [];
            for (var i = 0; i < globalData.length; i++) {
                var tr = document.createElement('tr');
                var tdName = document.createElement('td');
                tdName.textContent = globalData[i][0];
                var tdCount = document.createElement('td');
                tdCount.className = 'count';
                tdCount.textContent = globalData[i][1];
                var tdMax = document.createElement('td');
                tdMax.className = 'count';
                tdMax.textContent = globalData[i][2];
                tr.appendChild(tdName);
                tr.appendChild(tdCount);
                tr.appendChild(tdMax);
                tbody.appendChild(tr);
            }

            // Per-case distribution (sorted by max latency desc)
            var perCaseDiv = document.getElementById('perCaseDist');
            perCaseDiv.innerHTML = '';
            var perCase = distData.per_case || {};
            var caseNames = Object.keys(perCase).sort();
            for (var ci = 0; ci < caseNames.length; ci++) {
                var cn = caseNames[ci];
                var entries = perCase[cn];
                var h3 = document.createElement('h3');
                h3.className = 'toggle-header';
                h3.textContent = cn;
                h3.onclick = function() { toggleSection(this); };
                perCaseDiv.appendChild(h3);
                var contentDiv = document.createElement('div');
                contentDiv.className = 'toggle-content';
                var table = document.createElement('table');
                var thead = document.createElement('thead');
                thead.innerHTML = '<tr><th>Sub-path</th><th>Count</th><th>Max RTT [ns]</th></tr>';
                table.appendChild(thead);
                var tbody2 = document.createElement('tbody');
                for (var i = 0; i < entries.length; i++) {
                    var tr = document.createElement('tr');
                    var td1 = document.createElement('td');
                    td1.textContent = entries[i][0];
                    var td2 = document.createElement('td');
                    td2.className = 'count';
                    td2.textContent = entries[i][1];
                    var td3 = document.createElement('td');
                    td3.className = 'count';
                    td3.textContent = entries[i][2];
                    tr.appendChild(td1);
                    tr.appendChild(td2);
                    tr.appendChild(td3);
                    tbody2.appendChild(tr);
                }
                table.appendChild(tbody2);
                contentDiv.appendChild(table);
                perCaseDiv.appendChild(contentDiv);
            }
        }

        window.onload = function() {
            var caseSel = document.getElementById('caseSelector');
            if (caseSel.options.length > 0) {
                caseSel.selectedIndex = 0;
                updateSubPaths();
            }
        };
    </script>
</body>
</html>"""
    html_content = html_content.replace('__CASE_OPTIONS__', case_options)
    html_content = html_content.replace('__CASES_JSON__', json.dumps(cases_data, indent=2))
    html_content = html_content.replace('__DIST_JSON__', json.dumps(dist_data, indent=2))

    with open(summary_path, 'w', encoding='utf-8') as f:
        f.write(html_content)

    print('\nSummary HTML -> %s' % summary_path)
    return summary_path


def main():
    parser = argparse.ArgumentParser(
        description='Batch visualize URMA perf logs.')
    parser.add_argument('patha', help='Root directory containing urma_perf.log files')
    args = parser.parse_args()

    patha_abs = os.path.abspath(args.patha)
    if not os.path.isdir(patha_abs):
        print('Error: patha "%s" is not a directory' % patha_abs, file=sys.stderr)
        sys.exit(1)

    log_paths = find_log_files(patha_abs)
    if not log_paths:
        print('No urma_perf.log files found under %s' % patha_abs, file=sys.stderr)
        sys.exit(1)

    print('Found %d urma_perf.log file(s):' % len(log_paths))
    print('Processing with %d worker processes\n' % MAX_WORKERS)

    # Process logs in parallel. as_completed gives live progress; results are
    # stored per path and re-aggregated in log_paths order below so the
    # summary (dropdown order, top-200 data) is deterministic.
    results = {}  # log_path -> (success, rtt_values)
    with concurrent.futures.ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        future_to_path = {executor.submit(process_log, lp): lp for lp in log_paths}
        done = 0
        for future in concurrent.futures.as_completed(future_to_path):
            lp = future_to_path[future]
            done += 1
            try:
                success, rtt_values, msgs = future.result()
            except Exception as exc:  # worker crashed: record and keep going
                success, rtt_values, msgs = False, [], ['  Error: %r' % exc]
            print('[%d/%d] %s' % (done, len(log_paths), lp))
            for m in msgs:
                print(m)
            print()
            results[lp] = (success, rtt_values)

    processed = []
    all_rtt_data = []  # (value, case_name, sub_display, full_display)
    for log_path in log_paths:
        success, rtt_values = results[log_path]
        if not success:
            continue
        processed.append(log_path)
        log_dir = os.path.dirname(log_path)
        rel_to_patha = os.path.relpath(log_dir, patha_abs)
        parts = rel_to_patha.split(os.sep)
        case_name = parts[0]
        sub_path = os.sep.join(parts[1:]) if len(parts) > 1 else ''
        sub_display = sub_path.replace(os.sep, '/') if sub_path else '(root)'
        full_display = (case_name + '/' + sub_display) if sub_path else case_name
        for val in rtt_values:
            all_rtt_data.append((val, case_name, sub_display, full_display))

    print('Generating summary HTML...')
    generate_summary_html(patha_abs, processed, all_rtt_data)

    print('\nDone. Processed %d log file(s).' % len(processed))


if __name__ == '__main__':
    main()
