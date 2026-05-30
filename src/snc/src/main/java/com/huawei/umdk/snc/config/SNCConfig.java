package com.huawei.umdk.snc.config;

/**
 * SNC 全局配置
 */
public class SNCConfig {
    /** 日志级别 */
    private String logLevel = "INFO";

    public SNCConfig() {
    }

    public String getLogLevel() {
        return logLevel;
    }

    public void setLogLevel(String logLevel) {
        this.logLevel = logLevel;
    }

    @Override
    public String toString() {
        return "SNCConfig{" +
               ", logLevel='" + logLevel + '\'' +
               '}';
    }
}