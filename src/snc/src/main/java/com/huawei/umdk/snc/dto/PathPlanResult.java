/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: OpenCode
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
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
        SUCCESS(0, "鎴愬姛"),
        SRC_INFO_ERR(1003, "婧愪俊鎭敊璇?),
        DST_INFO_ERR(1004, "鐩殑淇℃伅閿欒"),
        ACL_CHECK_FAILED(1005, "ACL妫€鏌ュけ璐?),
        TOPO_INCOMPLETE(1007, "鎷撴墤淇℃伅涓嶅畬鏁?),
        TOPO_CONNECTION_ERROR(1008, "鎷撴墤杩炴帴閿欒"),
        TOPO_CONNECTION_NOT_FOUND(1009, "鎷撴墤杩炴帴鏈壘鍒?),
        ROUTE_NOT_REACHABLE(1010, "璺敱涓嶅彲杈?),
        TOPO_NOT_FOUND(1012, "鎷撴墤鏈壘鍒?),
        ACL_NOT_FOUND(1013, "ACL鏈壘鍒?),
        SRC_AND_DST_MUST_BE_NPU(3002, "婧愬拰鐩殑蹇呴』涓篘PU"),
        UPI_MISMATCH(3003, "UPI涓嶅尮閰?);

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
    private int ackUdpSrcPort;
    private int dataUdpSrcPort;
    private boolean spray;

    public PathPlanResult(PlanStatus status, String errorMessage) {
        this.status = status;
        this.errorMessage = errorMessage;
    }
}
