#!/usr/bin/env bash
set -euo pipefail

ADMIN_BIN="${ADMIN_BIN:-urma_admin}"
USE_SUDO=1

MAIN_UE_EID="2001:0db8:face:0001:0000:0000:dfdf:0001"
ALT_MAIN_UE_EID="2001:0db8:face:0002:0000:0000:dfdf:0002"
SINGLE_EID="2001:0db8:face:0001:0000:0000:dfdf:1001"
BATCH_NUM="${BATCH_NUM:-128}"
BATCH_CHUNK="${BATCH_CHUNK:-16}"

usage()
{
    cat <<'USAGE'
Usage: run_urma_admin_main_ue_eid_cli_test.sh [options]

Options:
  --admin <path>   urma_admin binary, default: urma_admin from PATH.
  --batch-num <n>  Total EIDs inserted by batch commands, default: 128.
  --batch-chunk <n>
                   EIDs per insert_batch command, default: 16.
  --no-sudo        Do not prefix commands with sudo.
  -h, --help       Show this help.

Environment:
  ADMIN_BIN        Same as --admin.
  BATCH_NUM        Same as --batch-num.
  BATCH_CHUNK      Same as --batch-chunk.

Test flow:
  1. flush table
  2. verify empty lookup fails
  3. insert one EID and lookup
  4. update the same EID and lookup
  5. delete it and verify lookup fails
  6. insert BATCH_NUM EIDs by insert_batch chunks and sample lookup
  7. delete one batch item and verify neighbors still exist
  8. flush table again
USAGE
}

log()
{
    printf '\n[admin-cli-test] %s\n' "$*"
}

die()
{
    printf '[admin-cli-test][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --admin)
            [[ $# -ge 2 ]] || die "--admin requires an argument"
            ADMIN_BIN="$2"
            shift 2
            ;;
        --no-sudo)
            USE_SUDO=0
            shift
            ;;
        --batch-num)
            [[ $# -ge 2 ]] || die "--batch-num requires an argument"
            BATCH_NUM="$2"
            shift 2
            ;;
        --batch-chunk)
            [[ $# -ge 2 ]] || die "--batch-chunk requires an argument"
            BATCH_CHUNK="$2"
            shift 2
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

if [[ "${EUID}" -eq 0 ]]; then
    USE_SUDO=0
fi

admin_cmd()
{
    local cmd=()

    if [[ "${USE_SUDO}" == "1" ]]; then
        cmd+=(sudo)
    fi
    cmd+=("${ADMIN_BIN}" main_ue_eid "$@")

    printf '[admin-cli-test] run:'
    printf ' %q' "${cmd[@]}"
    printf '\n'
    "${cmd[@]}"
}

lookup_output()
{
    local eid="$1"
    local output

    if ! output="$(admin_cmd lookup "${eid}" 2>&1)"; then
        printf '%s\n' "${output}"
        return 1
    fi

    printf '%s\n' "${output}" | tail -n 1
}

expect_lookup()
{
    local eid="$1"
    local expected="$2"
    local actual

    actual="$(lookup_output "${eid}")" || die "lookup failed for ${eid}"
    if [[ "${actual}" != "${expected}" ]]; then
        die "lookup mismatch for ${eid}: actual=${actual}, expected=${expected}"
    fi
    printf '[admin-cli-test] lookup matched: %s -> %s\n' "${eid}" "${actual}"
}

expect_lookup_absent()
{
    local eid="$1"
    local output

    if output="$(lookup_output "${eid}" 2>&1)"; then
        die "lookup unexpectedly succeeded for ${eid}: ${output}"
    fi
    printf '[admin-cli-test] lookup absent as expected: %s\n' "${eid}"
}

make_batch_eids()
{
    local -n out="$1"
    local i
    local eid

    out=()
    for i in $(seq 0 $((BATCH_NUM - 1))); do
        printf -v eid '2001:0db8:face:0003:0000:0000:dfdf:40%02x' "${i}"
        out+=("${eid}")
    done
}

insert_batch_chunks()
{
    local -n eids_ref="$1"
    local total="${#eids_ref[@]}"
    local start=0
    local end
    local chunk=()

    while [[ "${start}" -lt "${total}" ]]; do
        end=$((start + BATCH_CHUNK))
        if [[ "${end}" -gt "${total}" ]]; then
            end="${total}"
        fi

        chunk=("${eids_ref[@]:${start}:$((end - start))}")
        admin_cmd insert_batch "${MAIN_UE_EID}" "${chunk[@]}"
        start="${end}"
    done
}

main()
{
    local batch_eids=()
    local mid

    [[ "${BATCH_NUM}" =~ ^[0-9]+$ ]] || die "invalid BATCH_NUM: ${BATCH_NUM}"
    [[ "${BATCH_CHUNK}" =~ ^[0-9]+$ ]] || die "invalid BATCH_CHUNK: ${BATCH_CHUNK}"
    [[ "${BATCH_NUM}" -ge 3 ]] || die "BATCH_NUM must be >= 3"
    [[ "${BATCH_CHUNK}" -ge 1 ]] || die "BATCH_CHUNK must be >= 1"
    [[ "${BATCH_CHUNK}" -le 16 ]] || die "BATCH_CHUNK must be <= 16 to avoid urma_admin MAX_CMDLINE_LEN"

    mid=$((BATCH_NUM / 2))
    make_batch_eids batch_eids

    log "flush initial table"
    admin_cmd flush

    log "verify empty lookup fails"
    expect_lookup_absent "${SINGLE_EID}"

    log "single insert and lookup"
    admin_cmd insert "${SINGLE_EID}" "${MAIN_UE_EID}"
    expect_lookup "${SINGLE_EID}" "${MAIN_UE_EID}"

    log "single update and lookup"
    admin_cmd insert "${SINGLE_EID}" "${ALT_MAIN_UE_EID}"
    expect_lookup "${SINGLE_EID}" "${ALT_MAIN_UE_EID}"

    log "single delete"
    admin_cmd delete "${SINGLE_EID}"
    expect_lookup_absent "${SINGLE_EID}"

    log "batch insert ${BATCH_NUM} EIDs, ${BATCH_CHUNK} per command"
    insert_batch_chunks batch_eids

    log "sample batch lookup"
    expect_lookup "${batch_eids[0]}" "${MAIN_UE_EID}"
    expect_lookup "${batch_eids[${mid}]}" "${MAIN_UE_EID}"
    expect_lookup "${batch_eids[$((BATCH_NUM - 1))]}" "${MAIN_UE_EID}"

    log "delete one batch item and verify neighbors"
    admin_cmd delete "${batch_eids[${mid}]}"
    expect_lookup_absent "${batch_eids[${mid}]}"
    expect_lookup "${batch_eids[$((mid - 1))]}" "${MAIN_UE_EID}"
    expect_lookup "${batch_eids[$((mid + 1))]}" "${MAIN_UE_EID}"

    log "flush and verify no residue"
    admin_cmd flush
    expect_lookup_absent "${batch_eids[0]}"
    expect_lookup_absent "${batch_eids[$((BATCH_NUM - 1))]}"

    log "urma_admin main_ue_eid CLI test passed"
}

main "$@"
