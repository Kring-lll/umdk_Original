/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SNC (Super Node Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.dto;

import lombok.AllArgsConstructor;
import lombok.EqualsAndHashCode;
import lombok.Getter;
import lombok.NoArgsConstructor;
import lombok.Setter;
import lombok.ToString;

@Getter
@Setter
@NoArgsConstructor
@AllArgsConstructor
@EqualsAndHashCode
@ToString
public class PathPlanResult {

    public enum PlanStatus {
        SUCCESS(0, "成功"),
        SRC_INFO_ERR(1003, "源信息错�?),
        DST_INFO_ERR(1004, "目的信息错误"),
        ACL_CHECK_FAILED(1005, "ACL检查失�?),
        TOPO_INCOMPLETE(1007, "拓扑信息不完�?),
        TOPO_CONNECTION_ERROR(1008, "拓扑连接错误"),
        TOPO_CONNECTION_NOT_FOUND(1009, "拓扑连接未找�?),
        ROUTE_NOT_REACHABLE(1010, "路由不可�?),
        TOPO_NOT_FOUND(1012, "拓扑未找�?),
        ACL_NOT_FOUND(1013, "ACL未找�?),
        SRC_AND_DST_MUST_BE_NPU(3002, "源和目的必须为NPU"),
        UPI_MISMATCH(3003, "UPI不匹�?);

        @Getter
        private final int code;

        @Getter
        private final String message;

        PlanStatus(int code, String message) {
            this.code = code;
            this.message = message;
        }
    }

    private String srcEid;
    private String dstEid;
    private PathInfo path;
    private PlanStatus status;
    private String errorMessage;
    private Integer ackUdpSrcPort;
    private Integer dataUdpSrcPort;
    private Boolean spray;

    public PathPlanResult(PlanStatus status, String errorMessage) {
        this.status = status;
        this.errorMessage = errorMessage;
    }
}
