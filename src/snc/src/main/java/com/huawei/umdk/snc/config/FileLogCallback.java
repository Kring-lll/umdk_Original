/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-15
 * Note:
 * History: 2026-07-15  Create File
 */
package com.huawei.umdk.snc.config;

import java.io.IOException;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.EnumMap;
import java.util.Map;


public class FileLogCallback implements LogCallback {

    private final Path logDir;
    private final Map<LogLevel, PrintWriter> writers = new EnumMap<>(LogLevel.class);

    public FileLogCallback(String logDir) {
        this.logDir = Path.of(logDir);
        try {
            Files.createDirectories(this.logDir);
        } catch (IOException e) {
            throw new RuntimeException("Failed to create log directory: " + this.logDir, e);
        }

        for (LogLevel level : LogLevel.values()) {
            try {
                Path file = this.logDir.resolve(level.getValue() + ".log");
                PrintWriter pw = new PrintWriter(Files.newOutputStream(file), true);
                writers.put(level, pw);
            } catch (IOException e) {
                throw new RuntimeException("Failed to create log file for level: " + level, e);
            }
        }
    }

    @Override
    public void log(LogLevel level, String msg) {
        PrintWriter pw = writers.get(level);
        if (pw != null) {
            pw.println("[" + level.getValue() + "] " + msg);
        }
    }

    public void close() {
        for (PrintWriter pw : writers.values()) {
            pw.close();
        }
    }
}
