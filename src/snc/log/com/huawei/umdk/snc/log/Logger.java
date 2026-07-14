/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File; 2026-07-15 Add timestamp + credential sanitization
 */
package com.huawei.umdk.snc.log;

import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.Arrays;

import com.huawei.umdk.snc.entity.MgmtInfo;

public class Logger {
    private static volatile LogCallback callback;

    private final String tag;

    private static final String MASKED = "***";
    private static final DateTimeFormatter TIME_FMT = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss");

    public Logger(Class<?> clazz) {
        this.tag = clazz.getName();
    }

    public static void registerLogCallback(LogCallback callback) {
        Logger.callback = callback;
    }


    public static String mask(String value) {
        return value == null ? "null" : MASKED;
    }

    private String spliceLogInfo(String format, Object... args) {
        Object[] sanitized = sanitizeArgs(args);
        String timestamp = LocalDateTime.now().format(TIME_FMT);
        return "[" + timestamp + "] " + tag + ": " + String.format(format, sanitized);
    }

    private Object[] sanitizeArgs(Object[] args) {
        if (args == null || args.length == 0) {
            return args;
        }
        Object[] result = Arrays.copyOf(args, args.length);
        for (int i = 0; i < result.length; i++) {
            if (result[i] instanceof MgmtInfo) {
                result[i] = maskMgmtInfo((MgmtInfo) result[i]);
            }
        }
        return result;
    }

    private static String maskMgmtInfo(MgmtInfo info) {
        if (info == null) {
            return "null";
        }
        return "MgmtInfo(ip=***, port=***, username=***, password=***)";
    }

    private void log(LogLevel level, String msg) {
        if (callback != null) {
            callback.log(level, msg);
        }
    }

    public void debug(String format, Object... args) {
        log(LogLevel.DEBUG, spliceLogInfo(format, args));
    }

    public void info(String format, Object... args) {
        log(LogLevel.INFO, spliceLogInfo(format, args));
    }

    public void warn(String format, Object... args) {
        log(LogLevel.WARN, spliceLogInfo(format, args));
    }

    public void error(String format, Object... args) {
        log(LogLevel.ERROR, spliceLogInfo(format, args));
    }
}
