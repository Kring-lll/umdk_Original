/*
* SPDX-License-Identifier: MIT
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* Description: SNC (Supernode Network Controller) service
* Create: 2026-07-07
* Note:
* History: 2026-07-07  Create File
*/
package com.huawei.umdk.snc.config;

public class Logger {
    private static LogCallback callback;

    private final String tag;

    public Logger(Class<?> clazz){
        this.tag = clazz.getName();
    }

    public static void registerLogCallback(LogCallback callback){
        Logger.callback = callback;
    }

    private String spliceLogInfo(String format , Object... args){
        return (tag + ": " + String.format(format, args));
    }

    private void log(LogLevel level , String msg){
        if(callback != null){
            callback.log(level, msg);
        }
    }

    public void debug(String format , Object... args){
        log(LogLevel.DEBUG, spliceLogInfo(format, args));
    }

    public void info(String format , Object... args){
        log(LogLevel.INFO, spliceLogInfo(format, args));
    }

    
    public void warn(String format , Object... args){
        log(LogLevel.WARN, spliceLogInfo(format, args));
    }

    
    public void error(String format , Object... args){
        log(LogLevel.ERROR, spliceLogInfo(format, args));
    }
}