#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.wyh.conf}"
PRIMARY_EID="${PRIMARY_EID:-0000:0000:0000:0040:0010:0000:dfdf:004b}"
NODE_ID_BASE="${NODE_ID_BASE:-1000000}"
LOOKUP_THREADS="${LOOKUP_THREADS:-16}"
LOOKUP_LOOP="${LOOKUP_LOOP:-20}"
API_THREADS="${API_THREADS:-0}"
REQUIRE_EIDS="${REQUIRE_EIDS:-280000}"
WARN_EIDS="${WARN_EIDS:-280000}"
ADMIN_BIN="${ADMIN_BIN:-urma_admin}"
ADMIN_BATCH_NUM="${ADMIN_BATCH_NUM:-128}"
ADMIN_BATCH_CHUNK="${ADMIN_BATCH_CHUNK:-16}"
RUN_ADMIN_TEST=1
RUN_DEBUG_TEST=1
RUN_CONCURRENCY_TEST=1
RUN_WITH_SUDO=1

usage()
{
    cat <<'USAGE'
Usage: run_main_ue_eid_smoke_suite.sh [options]

Options:
  --config <file>        Test config file.
  --primary-eid <eid>    Real current-node primary EID for synthetic topo.
  --node-id-base <id>    Synthetic fake node id base, default 1000000.
  --lookup-threads <n>   Large-topo lookup threads, default 16.
  --lookup-loop <n>      Large-topo lookup loop, default 20.
  --api-threads <n>      Direct API stress threads in concurrency test, default 0.
  --require-eids <n>     Required query EID count, default 280000.
  --warn-eids <n>        Warning query EID threshold, default 280000.
  --admin <path>         urma_admin binary, default: urma_admin from PATH.
  --admin-batch-num <n>  Total EIDs inserted by admin batch test, default 128.
  --admin-batch-chunk <n>
                         EIDs per admin insert_batch command, default 16.
  --skip-admin-test      Skip urma_admin CLI path test.
  --skip-debug-test      Skip direct debug API test.
  --skip-concurrency     Skip large synthetic topo consistency test.
  --no-sudo              Pass --no-sudo to scripts that support it.
  -h, --help             Show this help.

Default flow:
  1. run_urma_admin_main_ue_eid_cli_test.sh
  2. run_main_primary_eid_debug_test.sh --flush-first --full-api
  3. run_main_primary_eid_concurrency_test.sh with large synthetic topo
USAGE
}

log()
{
    printf '\n[suite] %s\n' "$*"
}

die()
{
    printf '[suite][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --primary-eid)
            PRIMARY_EID="$2"
            shift 2
            ;;
        --node-id-base)
            NODE_ID_BASE="$2"
            shift 2
            ;;
        --lookup-threads)
            LOOKUP_THREADS="$2"
            shift 2
            ;;
        --lookup-loop)
            LOOKUP_LOOP="$2"
            shift 2
            ;;
        --api-threads)
            API_THREADS="$2"
            shift 2
            ;;
        --require-eids)
            REQUIRE_EIDS="$2"
            shift 2
            ;;
        --warn-eids)
            WARN_EIDS="$2"
            shift 2
            ;;
        --admin)
            ADMIN_BIN="$2"
            shift 2
            ;;
        --admin-batch-num)
            ADMIN_BATCH_NUM="$2"
            shift 2
            ;;
        --admin-batch-chunk)
            ADMIN_BATCH_CHUNK="$2"
            shift 2
            ;;
        --skip-admin-test)
            RUN_ADMIN_TEST=0
            shift
            ;;
        --skip-debug-test)
            RUN_DEBUG_TEST=0
            shift
            ;;
        --skip-concurrency)
            RUN_CONCURRENCY_TEST=0
            shift
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

ADMIN_SCRIPT="${SCRIPT_DIR}/run_urma_admin_main_ue_eid_cli_test.sh"
DEBUG_SCRIPT="${SCRIPT_DIR}/run_main_primary_eid_debug_test.sh"
CONCURRENCY_SCRIPT="${SCRIPT_DIR}/run_main_primary_eid_concurrency_test.sh"

[[ -f "${ADMIN_SCRIPT}" ]] || die "admin test script not found: ${ADMIN_SCRIPT}"
[[ -f "${DEBUG_SCRIPT}" ]] || die "debug test script not found: ${DEBUG_SCRIPT}"
[[ -f "${CONCURRENCY_SCRIPT}" ]] || die "concurrency test script not found: ${CONCURRENCY_SCRIPT}"

COMMON_SCRIPT_ARGS=(--config "${CONFIG_FILE}")
if [[ "${RUN_WITH_SUDO}" != "1" ]]; then
    COMMON_SCRIPT_ARGS+=(--no-sudo)
fi

ADMIN_SCRIPT_ARGS=(
    --admin "${ADMIN_BIN}"
    --batch-num "${ADMIN_BATCH_NUM}"
    --batch-chunk "${ADMIN_BATCH_CHUNK}"
)
if [[ "${RUN_WITH_SUDO}" != "1" ]]; then
    ADMIN_SCRIPT_ARGS+=(--no-sudo)
fi

if [[ "${RUN_ADMIN_TEST}" == "1" ]]; then
    log "urma_admin CLI path test"
    bash "${ADMIN_SCRIPT}" "${ADMIN_SCRIPT_ARGS[@]}"
fi

if [[ "${RUN_DEBUG_TEST}" == "1" ]]; then
    log "direct debug API test"
    bash "${DEBUG_SCRIPT}" "${COMMON_SCRIPT_ARGS[@]}" --flush-first --full-api
fi

if [[ "${RUN_CONCURRENCY_TEST}" == "1" ]]; then
    log "large synthetic topo consistency test"
    bash "${CONCURRENCY_SCRIPT}" \
        "${COMMON_SCRIPT_ARGS[@]}" \
        --large-synthetic-topo \
        --primary-eid "${PRIMARY_EID}" \
        --flush-first \
        --node-id-base "${NODE_ID_BASE}" \
        --lookup-threads "${LOOKUP_THREADS}" \
        --lookup-loop "${LOOKUP_LOOP}" \
        --api-threads "${API_THREADS}" \
        --shuffle \
        --require-eids "${REQUIRE_EIDS}" \
        --warn-eids "${WARN_EIDS}"
fi

log "main_ue_eid smoke suite passed"
