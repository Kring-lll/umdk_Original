#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
#
# 多进程 SEND 验证：1 server + 3 client 并发。验证 sim IPC 是按 eid 任意互联的对等模型。
# 前提：build_and_test.sh 已跑过（build_noasan 的 liburma + liburma_sim.so 都在）。
# 用法: bash /umdk/sim/urma/examples/run_ipc_multi_send.sh
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
export URMA_SIM_HW=1

echo "================================================"
echo "多进程：编 test_ipc_multi_send"
echo "================================================"
gcc -I$URMA_INC -I$URMA_COMMON_INC \
    -L/umdk/sim/urma/build -lurma_sim \
    -L$UA_CORE -lurma -L$UA_COMMON -lurma_common \
    -Wl,-rpath,/umdk/sim/urma/build -Wl,-rpath,$UA_CORE -Wl,-rpath,$UA_COMMON \
    /umdk/sim/urma/examples/test_ipc_multi_send.c -o /tmp/test_ipc_multi_send

echo
echo "================================================"
echo "多进程：跑 1 server + 3 client 并发 SEND"
echo "================================================"
echo "server=udma_sim0  client1=udma_sim1  client2=udma_sim2  client3=udma_sim3"
echo "成功标志：server 收齐 3 个 recv CQE，slot0/1/2 分别拿到 C1/C2/C3 前缀 + TEST PASS"
echo "------------------------------------------------"
rm -f /tmp/urma_sim_oob_multi

# server 后台先起，等它写 OOB
( /tmp/test_ipc_multi_send server > /tmp/multi_server.out 2>&1 ) &
SERVER_PID=$!
sleep 3

# 3 个 client 并发起
( /tmp/test_ipc_multi_send client 1 > /tmp/multi_c1.out 2>&1 ) &
( /tmp/test_ipc_multi_send client 2 > /tmp/multi_c2.out 2>&1 ) &
( /tmp/test_ipc_multi_send client 3 > /tmp/multi_c3.out 2>&1 ) &

# 等 client 们退出
wait
# 等 server 收齐 CQE 退出（最多再 15s）
for i in $(seq 1 150); do
    kill -0 $SERVER_PID 2>/dev/null || break
    sleep 0.1
done
kill -9 $SERVER_PID 2>/dev/null || true

echo "--- server 输出 ---"
grep -E "TEST|poll CQE|slot|got|posted|wrote OOB|clients" /tmp/multi_server.out
echo
echo "--- client 输出（post SEND 行）---"
grep -E "post SEND|send CQE" /tmp/multi_c1.out /tmp/multi_c2.out /tmp/multi_c3.out
echo
echo "================================================"
grep -E "TEST PASS" /tmp/multi_server.out && echo "多进程 SEND 测试 PASS" || echo "多进程 SEND 测试 FAIL（看 /tmp/multi_*.out）"
