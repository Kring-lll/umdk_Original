#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.conf}"
EXTRA_TEST_ARGS=()
SET_TOPO_BEFORE_TEST=0
SET_TOPO_EXTRA_ARGS=()
LARGE_SYNTHETIC_REQUIRE_EIDS=300000
LARGE_SYNTHETIC_NODE_ID_BASE=100000
FLUSH_FIRST=0
EXPECTED_TOPO_FILE="${MAIN_PRIMARY_EID_EXPECTED_TOPO_FILE:-}"
EXPECTED_TOPO_FILE_SET=0
LARGE_SYNTHETIC_TOPO=0
NODE_ID_BASE_SET=0

usage()
{
    cat <<'USAGE'
Usage: run_main_primary_eid_concurrency_test.sh [options]

Options:
  --config <file>       Use another config file.
  --lookup-threads <n>  Pass --lookup-threads to the test.
  --lookup-loop <n>     Pass --lookup-loop to the test.
  --api-threads <n>     Pass --api-threads to the test; 0 disables direct API stress.
  --api-loop <n>        Pass --api-loop to the test.
  --api-entries <n>     Pass --api-entries to the test.
  --api-progress        Print direct API stress phase progress.
  --flush-first         Flush table before refreshing it through uvs_set_topo_info.
  --skip-set            Do not refresh table; query EIDs still come from current topo.
  --expected-topo-file <file>
                        Read old-topo expected lookup pairs from a dumped topo file.
  --check-only          Only run sequential old/new consistency check.
  --require-eids <n>    Fail if current topo provides fewer query EIDs than n.
  --warn-eids <n>       Warn if current topo provides fewer query EIDs than n.
  --require-multi-primary-group
                        Fail if no node/entity/chip group has multiple primary EIDs.
  --shuffle             Shuffle query pairs before threaded lookup.
  --seed <num>          Shuffle seed.
  --set-synthetic-topo  Configure synthetic topo before running the test.
  --large-synthetic-topo Configure a large synthetic topo before running the test.
                        Implies --set-synthetic-topo --node-num 64
                        --entity-num 128 --devs-per-entity 2 --ports-per-ue 9
                        --node-id-base 100000 unless explicitly overridden,
                        and requires at least 300000 topo query EIDs.
  --primary-eid <eid>   Existing UB device primary EID for --set-synthetic-topo. Repeatable.
  --agg-eid <eid>       Aggregation EID forwarded to synthetic topo setup.
  --port-eid <eid>      Port EID forwarded to synthetic topo setup. Repeatable.
  --dev-idx <idx>       Device index forwarded to synthetic topo setup.
  --node-id <id>        Node id forwarded to synthetic topo setup.
  --entity-id <id>      Entity id forwarded to synthetic topo setup.
  --chip-id <id>        Chip id forwarded to synthetic topo setup.
  --die-id <id>         Die id forwarded to synthetic topo setup.
  --node-num <num>      Synthetic topo node count.
  --node-id-base <id>   Synthetic topo node id base.
  --super-node-id <id>  Synthetic topo super node id.
  --entity-id-base <id> Synthetic topo entity id base.
  --entity-num <num>    Synthetic topo entities per node.
  --devs-per-entity <n> Synthetic topo primary EIDs per node/entity/chip group.
  --ports-per-ue <num>  Synthetic topo port EIDs per UE.
  --no-sudo             Run test without sudo.
  -h, --help            Show this help.

Typical:
  ./run_main_primary_eid_concurrency_test.sh --config ./main_primary_eid_debug.wyh.conf
  ./run_main_primary_eid_concurrency_test.sh --config ./main_primary_eid_debug.wyh.conf --lookup-threads 16 --lookup-loop 100 --api-threads 8 --api-loop 32
  ./run_main_primary_eid_concurrency_test.sh --config ./main_primary_eid_debug.wyh.conf --set-synthetic-topo --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:000b --node-num 64 --entity-num 16 --ports-per-ue 8 --require-eids 4096 --shuffle
  ./run_main_primary_eid_concurrency_test.sh --config ./main_primary_eid_debug.wyh.conf --set-synthetic-topo --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:000b --node-num 2 --entity-num 1 --devs-per-entity 2 --ports-per-ue 1 --require-multi-primary-group --check-only
  ./run_main_primary_eid_concurrency_test.sh --config ./main_primary_eid_debug.wyh.conf --large-synthetic-topo --primary-eid 0000:0000:0000:0040:0010:0000:dfdf:000b --lookup-threads 16 --lookup-loop 20 --api-threads 0 --shuffle
USAGE
}

log()
{
    printf '[concurrency] %s\n' "$*"
}

die()
{
    printf '[concurrency][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --lookup-threads|--lookup-loop|--api-threads|--api-loop|--api-entries|\
        --require-eids|--warn-eids|--seed)
            EXTRA_TEST_ARGS+=("$1" "$2")
            shift 2
            ;;
        --expected-topo-file)
            EXPECTED_TOPO_FILE="$2"
            EXPECTED_TOPO_FILE_SET=1
            shift 2
            ;;
        --flush-first)
            FLUSH_FIRST=1
            shift
            ;;
        --skip-set|--check-only|--shuffle|--api-progress|\
        --require-multi-primary-group)
            EXTRA_TEST_ARGS+=("$1")
            shift
            ;;
        --set-synthetic-topo)
            SET_TOPO_BEFORE_TEST=1
            SET_TOPO_EXTRA_ARGS+=("--synthetic")
            shift
            ;;
        --large-synthetic-topo)
            SET_TOPO_BEFORE_TEST=1
            LARGE_SYNTHETIC_TOPO=1
            SET_TOPO_EXTRA_ARGS+=("--synthetic" "--node-num" "64" "--entity-num" "128")
            SET_TOPO_EXTRA_ARGS+=("--devs-per-entity" "2" "--ports-per-ue" "9")
            EXTRA_TEST_ARGS+=("--require-eids" "${LARGE_SYNTHETIC_REQUIRE_EIDS}")
            EXTRA_TEST_ARGS+=("--warn-eids" "${LARGE_SYNTHETIC_REQUIRE_EIDS}")
            shift
            ;;
        --node-id-base)
            NODE_ID_BASE_SET=1
            SET_TOPO_EXTRA_ARGS+=("$1" "$2")
            shift 2
            ;;
        --primary-eid|--agg-eid|--port-eid|--dev-idx|--node-id|--entity-id|--chip-id|--die-id|\
        --node-num|--super-node-id|--entity-id-base|--entity-num|\
        --devs-per-entity|--ports-per-ue)
            SET_TOPO_EXTRA_ARGS+=("$1" "$2")
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

if [[ "${#SET_TOPO_EXTRA_ARGS[@]}" -gt 0 && "${SET_TOPO_BEFORE_TEST}" != "1" ]]; then
    die "synthetic topo options require --set-synthetic-topo"
fi

if [[ "${LARGE_SYNTHETIC_TOPO}" == "1" && "${NODE_ID_BASE_SET}" != "1" ]]; then
    SET_TOPO_EXTRA_ARGS+=("--node-id-base" "${LARGE_SYNTHETIC_NODE_ID_BASE}")
fi

[[ -f "${CONFIG_FILE}" ]] || die "config file not found: ${CONFIG_FILE}"
# shellcheck source=/dev/null
source "${CONFIG_FILE}"

if [[ -f "${RUNTIME_ENV_FILE}" ]]; then
    # shellcheck source=/dev/null
    source "${RUNTIME_ENV_FILE}"
fi

: "${MAIN_UE_EID_TEST_ROOT:=${SCRIPT_DIR}}"
: "${CONCURRENCY_TEST_SRC:=${MAIN_UE_EID_TEST_ROOT}/main_primary_eid_concurrency_test.c}"
: "${CONCURRENCY_TEST_BIN:=/tmp/main_primary_eid_concurrency_test}"
: "${CONCURRENCY_TEST_ARGS:=}"

if [[ "${SET_TOPO_BEFORE_TEST}" == "1" ]]; then
    SET_TOPO_SCRIPT="${SCRIPT_DIR}/run_main_primary_eid_set_topo.sh"
    [[ -f "${SET_TOPO_SCRIPT}" ]] || die "set topo script not found: ${SET_TOPO_SCRIPT}"

    SET_TOPO_CMD=(bash "${SET_TOPO_SCRIPT}" --config "${CONFIG_FILE}")
    if [[ "${RUN_WITH_SUDO}" != "1" ]]; then
        SET_TOPO_CMD+=("--no-sudo")
    fi
    if [[ -z "${EXPECTED_TOPO_FILE}" ]]; then
        EXPECTED_TOPO_FILE="/tmp/main_primary_eid_expected_topo.$$"
    fi
    SET_TOPO_CMD+=("--dump-topo" "${EXPECTED_TOPO_FILE}")
    if [[ "${FLUSH_FIRST}" == "1" ]]; then
        SET_TOPO_CMD+=("--flush-first")
    fi
    SET_TOPO_CMD+=("${SET_TOPO_EXTRA_ARGS[@]}")
    EXTRA_TEST_ARGS+=("--expected-topo-file" "${EXPECTED_TOPO_FILE}")

    log "configure synthetic topo before concurrency test"
    "${SET_TOPO_CMD[@]}"
else
    if [[ "${EXPECTED_TOPO_FILE_SET}" == "1" ]]; then
        EXTRA_TEST_ARGS+=("--expected-topo-file" "${EXPECTED_TOPO_FILE}")
    fi
    if [[ "${FLUSH_FIRST}" == "1" ]]; then
        EXTRA_TEST_ARGS+=("--flush-first")
    fi
fi

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
    [[ -n "${LIBTPSA_DIR}" ]] || die "cannot find libtpsa.so. Run prepare_main_primary_eid_debug_env.sh first."
fi

[[ -f "${CONCURRENCY_TEST_SRC}" ]] || die "test source not found: ${CONCURRENCY_TEST_SRC}"
[[ -d "${LIBTPSA_DIR}" ]] || die "LIBTPSA_DIR does not exist: ${LIBTPSA_DIR}"

log "compile ${CONCURRENCY_TEST_SRC}"
gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra -pthread \
    -I"${UMDK_REPO}/src/urma/lib/uvs/core/include" \
    -I"${UMDK_REPO}/src/urma/common/include" \
    -I"${UMDK_REPO}/src/urma/lib/urma/core/include" \
    "${CONCURRENCY_TEST_SRC}" \
    -L"${LIBTPSA_DIR}" \
    -Wl,-rpath,"${LIBTPSA_DIR}" \
    -Wl,-rpath-link,"${LIBTPSA_DIR}" \
    -ltpsa \
    -o "${CONCURRENCY_TEST_BIN}"

log "linked libtpsa"
LD_LIBRARY_PATH="${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" ldd "${CONCURRENCY_TEST_BIN}" | grep -E 'tpsa|not found' || true

RUN_ARGS=()
if [[ -n "${CONCURRENCY_TEST_ARGS}" ]]; then
    # shellcheck disable=SC2206
    RUN_ARGS+=(${CONCURRENCY_TEST_ARGS})
fi
RUN_ARGS+=("${EXTRA_TEST_ARGS[@]}")

log "run ${CONCURRENCY_TEST_BIN} ${RUN_ARGS[*]:-}"
if [[ "${RUN_WITH_SUDO}" == "1" && "$(id -u)" != "0" ]]; then
    sudo env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${CONCURRENCY_TEST_BIN}" "${RUN_ARGS[@]}"
else
    env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${CONCURRENCY_TEST_BIN}" "${RUN_ARGS[@]}"
fi
