/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * URMA 设备仿真 - 启动入口（constructor）。
 *
 * 本文件只做一件事：在 urma_init 之前初始化 sim 状态（urma_sim_init 造临时设备目录）
 * + 按 URMA_SIM_HW 决定运行模式。方式 3（sim 当 provider）已废弃，见
 * deprecated/urma_sim_provider_mode3.c。当前唯一支持方式 1（仿真硬件）。
 *
 * 方式 1（URMA_SIM_HW=1，现行）：
 *   - 不注册 sim provider，真 udma provider 接管跑真代码。
 *   - urma_sim_hw_start() 起后台轮询线程（扫 doorbell pi → 解 WQE → READ/WRITE/SEND
 *     数据搬运（本进程 memcpy / 跨进程 IPC）→ 造 CQE）。
 *   - 设备枚举/sysfs/ioctl/mmap/ummu 拦截在 urma_sim.c / urma_sim_dev.c /
 *     urma_sim_hw.c / urma_sim_ummu.c，IPC 在 urma_sim_ipc.c。
 *   - 详见 README.md "方式 1：仿真硬件"。
 *
 * 不设 URMA_SIM_HW（方式 3 废弃后的行为）：
 *   - sim 不注册 provider，真 udma provider 接管但无硬件 → create_context 会失败。
 *   - 设备枚举（urma_admin show）仍可用（走 sim 的 sysfs 拦截），但建链/post 跑不通。
 *   - 如需重启方式 3，见 deprecated/urma_sim_provider_mode3.c 的恢复说明。
 */

#include "urma_sim.h"
#include "urma_sim_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* constructor：在 urma_init 之前初始化 sim。
 * 方式 1（URMA_SIM_HW=1）：起轮询线程，真 udma provider 接管。
 * 方式 3 已废弃：不再注册 sim provider。 */
__attribute__((constructor))
static void urma_sim_provider_ctor(void)
{
    urma_sim_init();   /* 加载 config + 造 /tmp 设备目录（opendir/readdir 用），幂等 */
    const char *hw = getenv("URMA_SIM_HW");
    if (hw != NULL && hw[0] == '1') {
        /* 方式 1：起轮询线程，真 udma 代码跑，sim 在 mmap/拦截/轮询层介入 */
        urma_sim_hw_start();
    }
    /* 未设 URMA_SIM_HW：方式 3 已废弃，不注册 sim provider。真 udma provider 会接管
     * 但无硬件，仅设备枚举可用。要跑 post 必须 URMA_SIM_HW=1。 */
}
