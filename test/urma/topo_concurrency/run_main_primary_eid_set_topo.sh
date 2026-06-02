#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.conf}"
EXTRA_TEST_ARGS=()

usage()
{
    cat <<'USAGE'
Usage: run_main_primary_eid_set_topo.sh [options] --primary-eid <eid> [--synthetic]

Options:
  --config <file>       Use another config file.
  --primary-eid <eid>   Existing UB device primary EID. Repeatable with --synthetic.
  --agg-eid <eid>       Aggregation EID. Default is derived from primary EID.
  --port-eid <eid>      Add one port EID. Repeatable.
  --dev-idx <idx>       agg_devs index. Manual default 255, synthetic default 0.
  --node-id <id>        node_id. Default 0.
  --entity-id <id>      entity_id. Default 0.
  --chip-id <id>        chip_id. Default 0.
  --die-id <id>         die_id. Default 0.
  --synthetic           Generate diversified non-current topo nodes.
  --node-num <num>      Synthetic node count. Default 1; values greater than 1 add non-current nodes.
  --node-id-base <id>   Synthetic node_id base. Default 0.
  --super-node-id <id>  Synthetic super_node_id. Default 0.
  --entity-id-base <id> Synthetic entity_id base. Default 0.
  --entity-num <num>    Synthetic entities per node. Default 8.
  --devs-per-entity <n> Synthetic real primary EIDs per current-node entity/chip group. Default 6.
  --ports-per-ue <num>  Synthetic port EIDs per UE. Default 4.
  --dump-topo <file>    Dump generated topo for old-topo expected lookup.
  --flush-first         Flush main_ue_eid table before set_topo.
  --no-sudo             Run without sudo.
  -h, --help            Show this help.

Typical:
  ./run_main_primary_eid_set_topo.sh --config ./main_primary_eid_debug.wyh.conf --primary-eid 2001:db8::1
  ./run_main_primary_eid_set_topo.sh --config ./main_primary_eid_debug.wyh.conf --primary-eid 2001:db8::1 --synthetic
  ./run_main_primary_eid_set_topo.sh --config ./main_primary_eid_debug.wyh.conf --synthetic --primary-eid 2001:db8::1 --primary-eid 2001:db8::2
USAGE
}

log()
{
    printf '[set-topo] %s\n' "$*"
}

die()
{
    printf '[set-topo][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --primary-eid|--agg-eid|--port-eid|--dev-idx|--node-id|--entity-id|--chip-id|--die-id|\
        --node-num|--node-id-base|--super-node-id|--entity-id-base|--entity-num|\
        --devs-per-entity|--ports-per-ue|--dump-topo)
            EXTRA_TEST_ARGS+=("$1" "$2")
            shift 2
            ;;
        --synthetic|--flush-first)
            EXTRA_TEST_ARGS+=("$1")
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

: "${MAIN_UE_EID_TEST_ROOT:=${SCRIPT_DIR}}"
: "${SET_TOPO_SRC:=${MAIN_UE_EID_TEST_ROOT}/main_primary_eid_set_topo.c}"
: "${SET_TOPO_BIN:=/tmp/main_primary_eid_set_topo}"
: "${SET_TOPO_ARGS:=}"

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

[[ -f "${SET_TOPO_SRC}" ]] || die "set topo source not found: ${SET_TOPO_SRC}"
[[ -d "${LIBTPSA_DIR}" ]] || die "LIBTPSA_DIR does not exist: ${LIBTPSA_DIR}"

log "compile ${SET_TOPO_SRC}"
gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra \
    -I"${UMDK_REPO}/src/urma/lib/uvs/core/include" \
    -I"${UMDK_REPO}/src/urma/common/include" \
    -I"${UMDK_REPO}/src/urma/lib/urma/core/include" \
    "${SET_TOPO_SRC}" \
    -L"${LIBTPSA_DIR}" \
    -Wl,-rpath,"${LIBTPSA_DIR}" \
    -Wl,-rpath-link,"${LIBTPSA_DIR}" \
    -ltpsa \
    -o "${SET_TOPO_BIN}"

RUN_ARGS=()
if [[ -n "${SET_TOPO_ARGS}" ]]; then
    # shellcheck disable=SC2206
    RUN_ARGS+=(${SET_TOPO_ARGS})
fi
RUN_ARGS+=("${EXTRA_TEST_ARGS[@]}")

log "run ${SET_TOPO_BIN} ${RUN_ARGS[*]:-}"
if [[ "${RUN_WITH_SUDO}" == "1" && "$(id -u)" != "0" ]]; then
    sudo env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${SET_TOPO_BIN}" "${RUN_ARGS[@]}"
else
    env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${SET_TOPO_BIN}" "${RUN_ARGS[@]}"
fi
