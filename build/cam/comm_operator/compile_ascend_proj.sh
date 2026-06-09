#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
# Description: cam building script - migrated to npu_op_* build system
# Create: 2025-07-20
# History: 2025-07-20 create cam building script
#          2026-06-08 migrate from legacy opbuild/msopgen to npu_op_* build system

set -e

# 定义全局屏蔽列表
exclude_list=()
if [ -z "${SHMEM_HOME_PATH}" ]; then
    echo "Skipping shmem (SHMEM_HOME_PATH not set)"
    exclude_list+=("moe_combine_shmem" "moe_dispatch_shmem")
fi

CopyOps() {
    local src_dir="$1" # 源目录
    local dst_dir="$2" # 目标目录

    # 确保目标目录的op_host和op_kernel存在
    mkdir -p "$dst_dir/op_host" "$dst_dir/op_kernel"

    # 遍历源目录下所有直接子目录 （包括含空格目录）
    find "$src_dir" -mindepth 1 -maxdepth 1 -type d -print0 | while IFS= read -r -d '' subdir; do
        # 检查子目录是否存在（双重验证）
        subdir_name=$(basename "$subdir")

        if [ -d "$subdir" ]; then
            # 检查当前子目录是否在屏蔽列表中
            skip=false
            for excluded_dir in "${exclude_list[@]}"; do
                if [ "$subdir_name" = "$excluded_dir" ]; then
                    skip=true
                    break
                fi
            done

            # 如果在屏蔽列表中，则跳过处理
            if [ "$skip" = true ]; then
                continue
            fi

            # 处理op_host目录
            if [ -d "$subdir/op_host" ]; then
                cp -rf "$subdir/op_host/"* "$dst_dir/op_host/"
            fi

            # 处理op_kernel目录
            if [ -d "$subdir/op_kernel" ]; then
                cp -rf "$subdir/op_kernel/"* "$dst_dir/op_kernel/"
            fi
        fi
    done
}

# 构建算子工程并将其产物传到指定地点
BuildAscendProj() {
    local src_path=$1
    local soc_version=$2
    local is_extract=$3
    local build_type=$4
    local os_id=$(grep ^ID= /etc/os-release | cut -d= -f2 | tr -d '"')
    local arch=$(uname -m)
    local proj_name="ascend_kernels_${soc_version}_proj"

    cd "$src_path"

    # 确保 MODULE_BUILD_PATH 目录存在
    if [ ! -d "${MODULE_BUILD_PATH}" ]; then
        mkdir -p ${MODULE_BUILD_PATH}
    fi

    if [ -d "${MODULE_BUILD_PATH}/${proj_name}" ]; then
        rm -rf ${MODULE_BUILD_PATH}/${proj_name}
    fi

    # 创建新npu_op_*编译体系的项目目录
    mkdir -p ${MODULE_BUILD_PATH}/${proj_name}/op_host
    mkdir -p ${MODULE_BUILD_PATH}/${proj_name}/op_kernel

    # 复制顶层CMakeLists.txt和CMakePresets.json
    cp ./ascend_kernels/CMakeLists.txt ${MODULE_BUILD_PATH}/${proj_name}/
    cp ./ascend_kernels/CMakePresets.json ${MODULE_BUILD_PATH}/${proj_name}/

    # 复制op_host和op_kernel的CMakeLists.txt（新npu_op_*版本）
    cp ./ascend_kernels/cmake_files/op_host/CMakeLists.txt ${MODULE_BUILD_PATH}/${proj_name}/op_host/
    cp ./ascend_kernels/cmake_files/op_kernel/CMakeLists.txt ${MODULE_BUILD_PATH}/${proj_name}/op_kernel/

    # 复制所有算子的op_host源文件
    CopyOps "./ascend_kernels" "${MODULE_BUILD_PATH}/${proj_name}"

    # 复制pregen目录（包含预生成的aclnn stub文件）
    cp -rf ./ascend_kernels/pregen ${MODULE_BUILD_PATH}/${proj_name}/

    # 如果不需要编译shmem算子，移除相关的pregen文件
    if [ -z "${SHMEM_HOME_PATH}" ]; then
        rm -f ${MODULE_BUILD_PATH}/${proj_name}/pregen/build_out/autogen/*shmem*
    fi

    # 设置build_type到CMakePresets.json
    python3 $SCRIPTS_PATH/comm_operator/set_conf.py ${MODULE_BUILD_PATH}/${proj_name}/CMakePresets.json $build_type True CAM

    # CANN package path: try setenv first, then derive from ASCEND_TOOLKIT_HOME
    if [ -z "${ASCEND_CANN_PACKAGE_PATH}" ]; then
        if [ -n "${ASCEND_TOOLKIT_HOME}" ]; then
            export ASCEND_CANN_PACKAGE_PATH="${ASCEND_TOOLKIT_HOME}"
        else
            export ASCEND_CANN_PACKAGE_PATH="/usr/local/Ascend/ascend-toolkit/latest"
        fi
    fi

    # Export CATLASS_HOME_PATH for kernel compilation if CPATH contains catlass
    if [ -z "${CATLASS_HOME_PATH}" ] && echo "${CPATH}" | grep -q "catlass"; then
        export CATLASS_HOME_PATH=$(echo "${CPATH}" | tr ':' '\n' | grep "catlass" | sed 's|/include||')
    fi

    cd ${MODULE_BUILD_PATH}/${proj_name}

    # Configure cmake
    cmake -S . -B build_out --preset=default -DCMAKE_BUILD_TYPE=$build_type

    # Build the compile options string for kernel compilation.
    # The npu_op_* build system's ascendc_impl_build.py generates .py files with
    # op_compile_option = '{}' (empty). We first run the copy_kernel_srcs target
    # to generate .py files, patch them, then run the rest of the build.
    KERNEL_OPTS=""
    if [ -n "${SHMEM_HOME_PATH}" ]; then
        KERNEL_OPTS="-I${SHMEM_HOME_PATH}/shmem/include -I${SHMEM_HOME_PATH}/shmem/src/device"
    fi
    if [ -n "${CATLASS_HOME_PATH}" ]; then
        KERNEL_OPTS="${KERNEL_OPTS:+${KERNEL_OPTS} }-I${CATLASS_HOME_PATH}/include"
    fi
    if [ -n "${KERNEL_OPTS}" ]; then
        echo "Kernel compile options: ${KERNEL_OPTS}"
        # First generate the kernel source files (.py adapters) without compiling
        cmake --build build_out --target ascendc_kernels_copy_kernel_srcs --parallel 16
        # Patch generated .py files to include custom compile options for OPC PRECOMPILE
        # Replace "op_compile_option = '{}'" with actual include paths
        find build_out/op_kernel -name "*.py" -path "*/dynamic/*" -print0 | while IFS= read -r -d '' pyf; do
            sed -i "s|op_compile_option = '{}'|op_compile_option = {'__all__': ['${KERNEL_OPTS}']}|" "$pyf"
        done
    fi

    # 使用新的npu_op_* cmake构建系统
    cmake --build build_out --target binary -j16
    cmake --build build_out --target package -j16

    # 根据is_extract判断是否抽取run包
    if [ $is_extract -eq 1 ]; then
        if [ ! -d "$BUILD_OUT_PATH/comm_operator/extract" ]; then
            mkdir -p "$BUILD_OUT_PATH/comm_operator/extract"
        fi
        mkdir -p ${BUILD_OUT_PATH}/comm_operator/extract/${soc_version}
        build_out/*.run --extract=${BUILD_OUT_PATH}/comm_operator/extract/${soc_version}
    else
        cp build_out/*.run ${BUILD_OUT_PATH}/comm_operator/run/CAM_${soc_version}_${os_id}_${arch}.run
    fi
}

BuildAscendProj $1 $2 $3 $4
