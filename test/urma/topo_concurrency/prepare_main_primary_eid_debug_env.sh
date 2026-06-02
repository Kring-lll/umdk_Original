#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${CONFIG_FILE:-${SCRIPT_DIR}/main_primary_eid_debug.conf}"

usage()
{
    cat <<'USAGE'
Usage: prepare_main_primary_eid_debug_env.sh [options]

Options:
  --config <file>       Use another config file.
  --build-kernel        Build URMA kernel modules. Default from config.
  --no-build-kernel     Skip kernel module build.
  --load-modules        Load freshly built kernel modules.
  --no-load-modules     Skip loading kernel modules. Default from config.
  --force-reload        rmmod old modules before insmod new modules.
  --load-extra          Also load ubagg/uburma/ipourma if their .ko files exist.
  --build-umdk          Build UMDK with cmake/make. Default from config.
  --no-build-umdk       Skip UMDK build and use LIBTPSA_DIR from config/env.
  --install-umdk        Run cmake install after UMDK build.
  -h, --help            Show this help.

Typical:
  ./prepare_main_primary_eid_debug_env.sh
  ./prepare_main_primary_eid_debug_env.sh --load-modules --force-reload
USAGE
}

log()
{
    printf '[prepare] %s\n' "$*"
}

die()
{
    printf '[prepare][error] %s\n' "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)
            CONFIG_FILE="$2"
            shift 2
            ;;
        --build-kernel)
            BUILD_KERNEL_MODULES=1
            shift
            ;;
        --no-build-kernel)
            BUILD_KERNEL_MODULES=0
            shift
            ;;
        --load-modules)
            LOAD_KERNEL_MODULES=1
            shift
            ;;
        --no-load-modules)
            LOAD_KERNEL_MODULES=0
            shift
            ;;
        --force-reload)
            FORCE_RELOAD_MODULES=1
            shift
            ;;
        --load-extra)
            LOAD_EXTRA_URMA_MODULES=1
            shift
            ;;
        --build-umdk)
            BUILD_UMDK=1
            shift
            ;;
        --no-build-umdk)
            BUILD_UMDK=0
            shift
            ;;
        --install-umdk)
            INSTALL_UMDK_LIBS=1
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

require_dir()
{
    [[ -d "$1" ]] || die "directory not found: $1"
}

require_file()
{
    [[ -f "$1" ]] || die "file not found: $1"
}

kernel_config_value()
{
    local config_file="/boot/config-$(uname -r)"

    if [[ -f "${config_file}" ]]; then
        grep '^CONFIG_UB_URMA=' "${config_file}" || true
    fi
}

build_kernel_modules()
{
    require_dir "${KERNEL_REPO}"
    require_dir "${KERNEL_BUILD_DIR}"
    require_dir "${KERNEL_M_DIR}"

    log "build kernel modules: make -C ${KERNEL_BUILD_DIR} M=${KERNEL_M_DIR}"
    make -C "${KERNEL_BUILD_DIR}" \
        M="${KERNEL_M_DIR}" \
        EXTRA_CFLAGS="${KERNEL_EXTRA_CFLAGS}" \
        modules \
        -j"$(nproc)"
}

safe_rmmod()
{
    local mod="$1"

    if lsmod | awk '{print $1}' | grep -qx "${mod}"; then
        log "rmmod ${mod}"
        sudo rmmod "${mod}"
    fi
}

module_loaded()
{
    local mod="$1"

    lsmod | awk '{print $1}' | grep -qx "${mod}"
}

require_module_unloaded()
{
    local mod="$1"
    local users

    if ! module_loaded "${mod}"; then
        return 0
    fi

    users="$(lsmod | awk -v mod="${mod}" '$1 == mod {print $4}')"
    die "module ${mod} is still loaded, cannot insert the new .ko. Used by: ${users:-unknown}. Stop/unload dependent modules first."
}

safe_insmod()
{
    local mod="$1"
    local ko="$2"

    require_file "${ko}"
    if module_loaded "${mod}"; then
        if [[ "${FORCE_RELOAD_MODULES}" == "1" ]]; then
            die "module ${mod} is still loaded after --force-reload, skip using old module."
        fi
        log "module ${mod} already loaded, skip insmod"
        return 0
    fi

    log "insmod ${ko}"
    sudo insmod "${ko}"
}

load_kernel_modules()
{
    local config
    local ubcore_ko="${KERNEL_M_DIR}/ubcore/ubcore.ko"
    local ubagg_ko="${KERNEL_M_DIR}/ubagg/ubagg.ko"
    local uburma_ko="${KERNEL_M_DIR}/uburma/uburma.ko"
    local ipourma_ko="${KERNEL_M_DIR}/ulp/ipourma/ipourma.ko"

    config="$(kernel_config_value)"
    if [[ "${config}" == "CONFIG_UB_URMA=y" ]]; then
        die "CONFIG_UB_URMA=y, ubcore is built into kernel. Install/reboot a new kernel instead of insmod."
    fi

    if [[ "${FORCE_RELOAD_MODULES}" == "1" ]]; then
        safe_rmmod ipourma
        safe_rmmod uburma
        safe_rmmod ubagg
        safe_rmmod ubcore
        require_module_unloaded ubcore
    fi

    safe_insmod ubcore "${ubcore_ko}"

    if [[ "${LOAD_EXTRA_URMA_MODULES}" == "1" ]]; then
        [[ -f "${ubagg_ko}" ]] && safe_insmod ubagg "${ubagg_ko}"
        [[ -f "${uburma_ko}" ]] && safe_insmod uburma "${uburma_ko}"
        [[ -f "${ipourma_ko}" ]] && safe_insmod ipourma "${ipourma_ko}"
    fi

    if [[ ! -e /dev/ubcore/ubcore ]]; then
        log "warning: /dev/ubcore/ubcore does not exist yet"
    fi
}

build_umdk()
{
    require_dir "${UMDK_SRC_DIR}"
    mkdir -p "${UMDK_BUILD_DIR}"

    log "configure UMDK: ${UMDK_BUILD_DIR}"
    # shellcheck disable=SC2086
    cmake -S "${UMDK_SRC_DIR}" -B "${UMDK_BUILD_DIR}" ${UMDK_CMAKE_OPTIONS}

    log "build UMDK"
    cmake --build "${UMDK_BUILD_DIR}" --parallel "$(nproc)"

    if [[ "${INSTALL_UMDK_LIBS}" == "1" ]]; then
        log "install UMDK build output"
        sudo cmake --build "${UMDK_BUILD_DIR}" --target install --parallel "$(nproc)"
    fi
}

find_libtpsa_dir()
{
    local found
    local dir

    if [[ -n "${LIBTPSA_DIR}" ]]; then
        [[ -d "${LIBTPSA_DIR}" ]] || die "LIBTPSA_DIR does not exist: ${LIBTPSA_DIR}"
        printf '%s\n' "${LIBTPSA_DIR}"
        return 0
    fi

    found="$(find "${UMDK_BUILD_DIR}" -name 'libtpsa.so*' -print 2>/dev/null | sort -V | head -n 1 || true)"
    if [[ -n "${found}" ]]; then
        dirname "${found}"
        return 0
    fi

    for dir in /usr/lib64 /usr/local/lib64 /usr/lib /usr/local/lib /lib64 /lib; do
        if [[ -f "${dir}/libtpsa.so" ]]; then
            printf '%s\n' "${dir}"
            return 0
        fi
    done

    die "cannot find libtpsa.so. Build UMDK or set LIBTPSA_DIR."
}

write_runtime_env()
{
    local lib_dir="$1"

    log "write runtime env: ${RUNTIME_ENV_FILE}"
    cat > "${RUNTIME_ENV_FILE}" <<EOF
export MAIN_UE_EID_TEST_ROOT="${MAIN_UE_EID_TEST_ROOT}"
export KERNEL_REPO="${KERNEL_REPO}"
export KERNEL_BUILD_DIR="${KERNEL_BUILD_DIR}"
export KERNEL_M_DIR="${KERNEL_M_DIR}"
export KERNEL_EXTRA_CFLAGS="${KERNEL_EXTRA_CFLAGS}"
export UMDK_REPO="${UMDK_REPO}"
export UMDK_SRC_DIR="${UMDK_SRC_DIR}"
export UMDK_BUILD_DIR="${UMDK_BUILD_DIR}"
export UMDK_CMAKE_OPTIONS="${UMDK_CMAKE_OPTIONS}"
export TEST_SRC="${TEST_SRC}"
export TEST_BIN="${TEST_BIN}"
export CONCURRENCY_TEST_SRC="${CONCURRENCY_TEST_SRC}"
export CONCURRENCY_TEST_BIN="${CONCURRENCY_TEST_BIN}"
export SET_TOPO_SRC="${SET_TOPO_SRC}"
export SET_TOPO_BIN="${SET_TOPO_BIN}"
export FTRACE_STRESS_SRC="${FTRACE_STRESS_SRC}"
export FTRACE_STRESS_BIN="${FTRACE_STRESS_BIN}"
export LIBTPSA_DIR="${lib_dir}"
export LD_LIBRARY_PATH="${lib_dir}:\${LD_LIBRARY_PATH:-}"
EOF
}

main()
{
    local lib_dir

    log "using config: ${CONFIG_FILE}"

    if [[ "${BUILD_KERNEL_MODULES}" == "1" ]]; then
        build_kernel_modules
    fi

    if [[ "${LOAD_KERNEL_MODULES}" == "1" ]]; then
        load_kernel_modules
    fi

    if [[ "${BUILD_UMDK}" == "1" ]]; then
        build_umdk
    fi

    lib_dir="$(find_libtpsa_dir)"
    write_runtime_env "${lib_dir}"

    log "done"
    log "source ${RUNTIME_ENV_FILE}"
    log "libtpsa dir: ${lib_dir}"
}

main "$@"
