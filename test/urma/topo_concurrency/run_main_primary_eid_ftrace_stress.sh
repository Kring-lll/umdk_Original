#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.conf}"
STRESS_ARGS=()
ENABLE_FTRACE=0
TRACE_OUT="${TRACE_OUT:-/tmp/main_primary_eid_ftrace.trace}"
TRACE_BUFFER_KB="${TRACE_BUFFER_KB:-0}"
TRACE_GRAPH_DEPTH="${TRACE_GRAPH_DEPTH:-0}"
TRACE_FUNC="${TRACE_FUNC:-}"

usage()
{
    cat <<'USAGE'
Usage: run_main_primary_eid_ftrace_stress.sh [options]

Options:
  --config <file>       Use another config file.
  --trace               Configure function_graph ftrace for main UE EID lookup.
  --trace-func <func>   Trace a specific kernel function. If omitted, the
                        script auto-selects an available main UE EID lookup
                        function from the current kernel.
  --trace-out <file>    Save ftrace output, default /tmp/main_primary_eid_ftrace.trace.
  --trace-buffer-kb <n> Set ftrace buffer_size_kb before tracing; 0 keeps current value.
  --trace-depth <n>     Set max_graph_depth; use 1 to trace only the target function.
                        0 keeps current value.
  --loop <n>            Stress loop count; 0 means run until Ctrl-C.
  --mode <mode>         Stress mode: set-topo or lookup. Default set-topo.
  --warmup <n>          Warmup set_topo calls before tracing workload.
  --print-every <n>     Print progress every n calls; 0 disables progress.
  --sleep-us <n>        Sleep between set_topo calls.
  --no-sudo             Run stress binary without sudo. --trace still requires root.
  -h, --help            Show this help.

Typical:
  sudo bash ./run_main_primary_eid_ftrace_stress.sh --config ./main_primary_eid_debug.wyh.conf --trace --loop 1000 --print-every 100
  sudo bash ./run_main_primary_eid_ftrace_stress.sh --config ./main_primary_eid_debug.wyh.conf --trace --trace-buffer-kb 65536 --trace-depth 1 --loop 1000
  sudo bash ./run_main_primary_eid_ftrace_stress.sh --config ./main_primary_eid_debug.wyh.conf --trace --loop 0 --sleep-us 1000
USAGE
}

log()
{
    printf '[ftrace-stress] %s\n' "$*"
}

die()
{
    printf '[ftrace-stress][error] %s\n' "$*" >&2
    exit 1
}

find_trace_dir()
{
    if [[ -d /sys/kernel/debug/tracing ]]; then
        printf '%s\n' /sys/kernel/debug/tracing
    elif [[ -d /sys/kernel/tracing ]]; then
        printf '%s\n' /sys/kernel/tracing
    else
        return 1
    fi
}

write_trace()
{
    local file="$1"
    local value="$2"

    if ! printf '%s\n' "${value}" > "${TRACE_DIR}/${file}"; then
        die "failed to write '${value}' to ${TRACE_DIR}/${file}"
    fi
}

trace_func_available()
{
    local func="$1"

    [[ -r "${TRACE_DIR}/available_filter_functions" ]] || return 1
    awk -v target_func="${func}" '$1 == target_func { found = 1; exit }
        END { exit found ? 0 : 1 }' "${TRACE_DIR}/available_filter_functions"
}

print_available_trace_funcs()
{
    [[ -r "${TRACE_DIR}/available_filter_functions" ]] || return

    log "available related ftrace functions:"
    grep -E 'ubcore_.*(main.*ue|main.*primary|eid.*trie.*lookup|lookup.*eid)' \
        "${TRACE_DIR}/available_filter_functions" || true
}

select_trace_func()
{
    local func
    local candidates=(
        ubcore_get_main_primary_eid
        ubcore_get_main_ue_eid
        ubcore_lookup_main_ue_eid
        ubcore_cmd_lookup_main_ue_eid
        ubcore_eid_trie_lookup
    )

    if [[ -n "${TRACE_FUNC}" ]]; then
        if trace_func_available "${TRACE_FUNC}"; then
            printf '%s\n' "${TRACE_FUNC}"
            return 0
        fi
        print_available_trace_funcs
        die "trace function is not available to ftrace: ${TRACE_FUNC}"
    fi

    for func in "${candidates[@]}"; do
        if trace_func_available "${func}"; then
            printf '%s\n' "${func}"
            return 0
        fi
    done

    print_available_trace_funcs
    die "no main UE EID lookup function is available to ftrace; check ${TRACE_DIR}/available_filter_functions"
}

setup_ftrace()
{
    local trace_func

    TRACE_DIR="$(find_trace_dir)" || die "cannot find ftrace directory"
    [[ "$(id -u)" == "0" ]] || die "--trace requires root; run this script with sudo"

    log "setup ftrace in ${TRACE_DIR}"
    trace_func="$(select_trace_func)"
    log "trace function: ${trace_func}"
    write_trace tracing_on 0
    write_trace current_tracer nop
    if [[ "${TRACE_BUFFER_KB}" != "0" ]]; then
        write_trace buffer_size_kb "${TRACE_BUFFER_KB}"
    fi
    : > "${TRACE_DIR}/trace"
    : > "${TRACE_DIR}/set_ftrace_filter"
    : > "${TRACE_DIR}/set_graph_function"
    if [[ "${TRACE_GRAPH_DEPTH}" != "0" && -f "${TRACE_DIR}/max_graph_depth" ]]; then
        write_trace max_graph_depth "${TRACE_GRAPH_DEPTH}"
    fi
    write_trace set_graph_function "${trace_func}"
    write_trace current_tracer function_graph
    if [[ -f "${TRACE_DIR}/available_options" ]] &&
        grep -qw funcgraph-duration "${TRACE_DIR}/available_options"; then
        write_trace trace_options funcgraph-duration
    fi
    write_trace tracing_on 1
}

stop_ftrace()
{
    if [[ "${ENABLE_FTRACE}" != "1" || -z "${TRACE_DIR:-}" ]]; then
        return
    fi

    write_trace tracing_on 0
    cp "${TRACE_DIR}/trace" "${TRACE_OUT}"
    write_trace current_tracer nop
    : > "${TRACE_DIR}/set_graph_function"
    log "ftrace saved to ${TRACE_OUT}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --trace)
            ENABLE_FTRACE=1
            shift
            ;;
        --trace-out)
            TRACE_OUT="$2"
            shift 2
            ;;
        --trace-func)
            TRACE_FUNC="$2"
            shift 2
            ;;
        --trace-buffer-kb)
            TRACE_BUFFER_KB="$2"
            shift 2
            ;;
        --trace-depth)
            TRACE_GRAPH_DEPTH="$2"
            shift 2
            ;;
        --loop|--mode|--warmup|--print-every|--sleep-us)
            STRESS_ARGS+=("$1" "$2")
            shift 2
            ;;
        --no-sudo)
            RUN_WITH_SUDO=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            die "unknown option: $1"
            ;;
    esac
done

[[ -f "${CONFIG_FILE}" ]] || die "config file not found: ${CONFIG_FILE}"
# shellcheck source=/dev/null
source "${CONFIG_FILE}"

if [[ -f "${RUNTIME_ENV_FILE}" ]]; then
    # shellcheck source=/dev/null
    source "${RUNTIME_ENV_FILE}"
fi

: "${MAIN_UE_EID_TEST_ROOT:=${SCRIPT_DIR}}"
: "${FTRACE_STRESS_SRC:=${MAIN_UE_EID_TEST_ROOT}/main_primary_eid_ftrace_stress.c}"
: "${FTRACE_STRESS_BIN:=/tmp/main_primary_eid_ftrace_stress}"

if [[ -z "${LIBTPSA_DIR}" ]]; then
    found="$(find "${UMDK_BUILD_DIR}" -name 'libtpsa.so*' -print 2>/dev/null | sort -V | head -n 1 || true)"
    if [[ -n "${found}" ]]; then
        LIBTPSA_DIR="$(dirname "${found}")"
    else
        for dir in /usr/lib64 /usr/local/lib64 /usr/lib /usr/local/lib /lib64 /lib; do
            if [[ -f "${dir}/libtpsa.so" ]]; then
                LIBTPSA_DIR="${dir}"
                break
            fi
        done
    fi
    [[ -n "${LIBTPSA_DIR}" ]] || die "cannot find libtpsa.so"
fi

[[ -f "${FTRACE_STRESS_SRC}" ]] || die "stress source not found: ${FTRACE_STRESS_SRC}"
[[ -d "${LIBTPSA_DIR}" ]] || die "LIBTPSA_DIR does not exist: ${LIBTPSA_DIR}"

log "compile ${FTRACE_STRESS_SRC}"
gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra \
    -I"${UMDK_REPO}/src/urma/lib/uvs/core/include" \
    -I"${UMDK_REPO}/src/urma/common/include" \
    -I"${UMDK_REPO}/src/urma/lib/urma/core/include" \
    "${FTRACE_STRESS_SRC}" \
    -L"${LIBTPSA_DIR}" \
    -Wl,-rpath,"${LIBTPSA_DIR}" \
    -Wl,-rpath-link,"${LIBTPSA_DIR}" \
    -ltpsa \
    -o "${FTRACE_STRESS_BIN}"

if [[ "${ENABLE_FTRACE}" == "1" ]]; then
    setup_ftrace
    trap stop_ftrace EXIT
fi

log "run ${FTRACE_STRESS_BIN} ${STRESS_ARGS[*]:-}"
if [[ "${RUN_WITH_SUDO}" == "1" && "$(id -u)" != "0" ]]; then
    sudo env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" \
        "${FTRACE_STRESS_BIN}" "${STRESS_ARGS[@]}"
else
    env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" \
        "${FTRACE_STRESS_BIN}" "${STRESS_ARGS[@]}"
fi
