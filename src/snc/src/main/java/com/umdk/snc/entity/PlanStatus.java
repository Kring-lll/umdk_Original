/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc plan status module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc plan status module
 */
package com.huawei.umdk.snc.entity;

/**
 * 路径规划状态枚举（错误码定义）
 *
 * 错误码编码规则：
 *   0    - 成功
 *   1    - 通用失败
 *   1xxx - 路径规划阶段错误
 *   3xxx - 参数校验错误
 */
public enum PlanStatus {
    /** 成功 */
    SUCCESS(0, "成功"),
    /** 通用失败 */
    FAILED(1, "通用失败"),

    // ---- 路径规划阶段错误 (1xxx) ----
    /** 源EID未找到（设备不存在） */
    SRC_EID_NOT_FOUND(1001, "源EID未找到"),
    /** 目的EID未找到（设备不存在） */
    DEST_EID_NOT_FOUND(1002, "目的EID未找到"),
    /** 源信息缺失或错误 */
    SRC_INFO_ERR(1003, "源信息错误"),
    /** 目的信息缺失或错误 */
    DST_INFO_ERR(1004, "目的信息错误"),
    /** ACL检查失败 */
    ACL_CHECK_FAILED(1005, "ACL检查失败"),
    /** 未找到路由 */
    NO_ROUTE_FOUND(1006, "未找到路由"),
    /** 拓扑不完整 */
    TOPO_INCOMPLETE(1007, "拓扑不完整"),
    /** 拓扑连接错误（直连场景不匹配） */
    TOPO_CONNECTION_ERROR(1008, "拓扑连接错误"),
    /** 未找到拓扑连接（多跳路径还原失败） */
    TOPO_CONNECTION_NOT_FOUND(1009, "未找到拓扑连接"),
    /** 路由不可达 */
    ROUTE_NOT_REACHABLE(1010, "路由不可达"),
    /** 存在多路径且设备不支持逐流 */
    MULTI_PATH_NOT_SUPPORTED(1011, "多路径不支持"),

    // ---- 参数校验错误 (3xxx) ----
    /** 源和目的设备类型必须为NPU */
    SRC_AND_DST_MUST_BE_NPU(3002, "源和目的设备类型必须为NPU"),

    // ---- 拓扑/ACL数据管理错误 (2xxx) ----
    /** 拓扑未找到 */
    TOPO_NOT_FOUND(2001, "拓扑未找到"),
    /** ACL未找到 */
    ACL_NOT_FOUND(2002, "ACL未找到"),
    /** ACL拒绝 */
    ACL_DENIED(2003, "ACL拒绝"),
    /** 路径未找到 */
    PATH_NOT_FOUND(2004, "路径未找到"),
    /** 无效参数 */
    INVALID_PARAM(3001, "无效参数");

    private final int code;
    private final String message;

    PlanStatus(int code, String message) {
        this.code = code;
        this.message = message;
    }

    public int getCode() {
        return code;
    }

    public String getMessage() {
        return message;
    }

    /**
     * 根据错误码查找对应的枚举值
     */
    public static PlanStatus fromCode(int code) {
        for (PlanStatus status : values()) {
            if (status.code == code) {
                return status;
            }
        }
        return FAILED;
    }

    /**
     * 判断是否成功
     */
    public boolean isSuccess() {
        return this == SUCCESS;
    }
}