/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc snc config test test module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc snc config test test module
 */
package com.huawei.umdk.snc.config;

import org.junit.jupiter.api.DisplayName;
import org.junit.jupiter.api.Nested;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

/**
 * SNCConfig 单元测试
 */
@DisplayName("SNCConfig 单元测试")
class SNCConfigTest {

    @Nested
    @DisplayName("默认值测试")
    class DefaultValuesTest {
        @Test
        @DisplayName("无参构造使用默认值")
        void testDefaultValues() {
            SNCConfig config = new SNCConfig();
            assertEquals("INFO", config.getLogLevel());
        }
    }

    @Nested
    @DisplayName("setLogLevel 测试")
    class LogLevelTest {
        @Test
        @DisplayName("设置为 DEBUG")
        void testSetLogLevelDebug() {
            SNCConfig config = new SNCConfig();
            config.setLogLevel("DEBUG");
            assertEquals("DEBUG", config.getLogLevel());
        }

        @Test
        @DisplayName("设置为 WARN")
        void testSetLogLevelWarn() {
            SNCConfig config = new SNCConfig();
            config.setLogLevel("WARN");
            assertEquals("WARN", config.getLogLevel());
        }

        @Test
        @DisplayName("设置为 null")
        void testSetLogLevelNull() {
            SNCConfig config = new SNCConfig();
            config.setLogLevel("INFO");
            config.setLogLevel(null);
            assertNull(config.getLogLevel());
        }
    }

    @Test
    @DisplayName("toString 包含关键字段")
    void testToString() {
        SNCConfig config = new SNCConfig();
        config.setLogLevel("DEBUG");
        String s = config.toString();
        assertTrue(s.contains("logLevel='DEBUG'"));
    }
}