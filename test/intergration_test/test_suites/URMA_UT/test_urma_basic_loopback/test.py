# -*- coding: utf-8 -*-
"""
* SPDX-License-Identifier: MIT
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* Description: urma basic loopback test refined
"""

import logging
from public import UBUSFeature

log = logging.getLogger()

class TestUrmaLoopback(UBUSFeature):

    def setup(self):
        super(TestUrmaLoopback, self).setup()
        log.info('---------- [ 测试环境初始化 ] ----------')

    def teardown(self):
        log.info('---------- [ 测试环境清理 ] ----------')
        super(TestUrmaLoopback, self).teardown()

    def test_urma_basic_loopback(self):
        """
        单机回环打流测试
        """
        if len(self.host_list) < 2:
            self.skipTest("主机列表不足，跳过测试")

        host1 = self.host_list[0]
        host2 = self.host_list[1]

        dev_name = "udma9"
        msg_size = 4096
        opt = f"--ctp -d {dev_name} -s {msg_size}"

        ret = self.urma_perftest_one_perf_single_dev(
            server=host1,
            client=host2,
            timeout=60,
            cmd_syntax="send_bw",
            opt=opt
        )

        self.assertEqual(ret, 0, f"回环打流测试失败，请检查网卡 {dev_name} 状态或底层驱动")
        log.info("---------- [ 单机回环打流测试通过 ] ----------")