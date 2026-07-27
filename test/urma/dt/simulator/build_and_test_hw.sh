#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 方式1（仿真硬件）端到端测试：URMA_SIM_HW=1 跑真 udma，sim 拦 mmap/ummu + 轮询。
# 前提：build_and_test.sh 已跑过（build_noasan 的 liburma + liburma_sim.so 都在）。
# 用法: bash /umdk/sim/urma/build_and_test_hw.sh
set -e

UA_CORE=/umdk/src/build_noasan/urma/lib/urma/core
UA_COMMON=/umdk/src/build_noasan/urma/common
SIM_SO=/umdk/sim/urma/build/liburma_sim.so
SIM_CFG=/umdk/sim/urma/urma_sim_config.json
URMA_INC=/umdk/src/urma/lib/urma/core/include
URMA_COMMON_INC=/umdk/src/urma/common/include

export LD_LIBRARY_PATH=$UA_CORE:$UA_COMMON
export LD_PRELOAD=$SIM_SO
export URMA_SIM_CONFIG=$SIM_CFG
export URMA_SIM_HW=1   # 方式1：停用 sim provider，真 udma 接管，sim 当硬件

echo "================================================"
echo "方式1：编 test_hw_post"
echo "================================================"
gcc -I$URMA_INC -I$URMA_COMMON_INC \
    -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
    -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
    /umdk/sim/urma/examples/test_hw_post.c -o /tmp/test_hw_post

echo
echo "================================================"
echo "方式1：跑 test_hw_post（READ 单进程端到端）"
echo "================================================"
echo "DBG 行出现 queue_assoc/cq_assoc/tid_seg 登记 + poll WQE + 造 CQE"
echo "成功标志：dst[0]='A' dst[end]='A' + poll_jfc n=1 status=0 + TEST PASS"
echo "------------------------------------------------"
/tmp/test_hw_post || echo "(test 退出码非0，看 DBG 输出排查)"

echo
echo "================================================"
echo "方式1：编 test_ipc_send（跨进程 SEND 端到端）"
echo "================================================"
gcc -I$URMA_INC -I$URMA_COMMON_INC \
    -L/umdk/sim/urma/build -lurma_sim \
    -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
    -Wl,-rpath,/umdk/sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
    /umdk/sim/urma/examples/test_ipc_send.c -o /tmp/test_ipc_send

echo
echo "================================================"
echo "方式1：跑 test_ipc_send（B SEND→A，双进程）"
echo "================================================"
echo "成功标志：[A] poll n=1 s_r=1 opcode=0 user_ctx=a1 + recv_buf='Hello from B' + [B] send CQE s_r=0 + TEST PASS"
echo "------------------------------------------------"
rm -f /tmp/urma_sim_oob_send
( /tmp/test_ipc_send A > /tmp/sendA.out 2>&1 ) &
sleep 3
/tmp/test_ipc_send B || echo "(test B 退出码非0，看 DBG 输出排查)"
sleep 1
grep -E "TEST|poll n|recv_buf1" /tmp/sendA.out | head

