#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.conf}"
EXTRA_TEST_ARGS=()

usage()
{
    cat <<'USAGE'
Usage: run_main_primary_eid_debug_test.sh [options]

Options:
  --config <file>    Use another config file.
  --flush-first      Pass --flush-first to the test binary.
  --keep             Pass --keep to the test binary.
  --full-api         Test batch set/delete/flush paths; this clears the table.
  --no-sudo          Run test without sudo.
  -h, --help         Show this help.

Typical:
  ./run_main_primary_eid_debug_test.sh
  ./run_main_primary_eid_debug_test.sh --flush-first
  ./run_main_primary_eid_debug_test.sh --flush-first --full-api
  ./run_main_primary_eid_debug_test.sh --keep
USAGE
}

log()
{
    printf '[test] %s\n' "$*"
}

die()
{
    printf '[test][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --flush-first)
            EXTRA_TEST_ARGS+=("--flush-first")
            shift
            ;;
        --keep)
            EXTRA_TEST_ARGS+=("--keep")
            shift
            ;;
        --full-api)
            EXTRA_TEST_ARGS+=("--full-api")
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

[[ -f "${TEST_SRC}" ]] || die "test source not found: ${TEST_SRC}"
[[ -d "${LIBTPSA_DIR}" ]] || die "LIBTPSA_DIR does not exist: ${LIBTPSA_DIR}"

log "compile ${TEST_SRC}"
gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra \
    -I"${UMDK_REPO}/src/urma/lib/uvs/core/include" \
    -I"${UMDK_REPO}/src/urma/common/include" \
    -I"${UMDK_REPO}/src/urma/lib/urma/core/include" \
    "${TEST_SRC}" \
    -L"${LIBTPSA_DIR}" \
    -Wl,-rpath,"${LIBTPSA_DIR}" \
    -Wl,-rpath-link,"${LIBTPSA_DIR}" \
    -ltpsa \
    -o "${TEST_BIN}"

log "linked libtpsa"
LD_LIBRARY_PATH="${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" ldd "${TEST_BIN}" | grep -E 'tpsa|not found' || true

RUN_ARGS=()
if [[ -n "${TEST_ARGS}" ]]; then
    # shellcheck disable=SC2206
    RUN_ARGS+=(${TEST_ARGS})
fi
RUN_ARGS+=("${EXTRA_TEST_ARGS[@]}")

log "run ${TEST_BIN} ${RUN_ARGS[*]:-}"
if [[ "${RUN_WITH_SUDO}" == "1" && "$(id -u)" != "0" ]]; then
    sudo env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${TEST_BIN}" "${RUN_ARGS[@]}"
else
    env "LD_LIBRARY_PATH=${LIBTPSA_DIR}:${LD_LIBRARY_PATH:-}" "${TEST_BIN}" "${RUN_ARGS[@]}"
fi
