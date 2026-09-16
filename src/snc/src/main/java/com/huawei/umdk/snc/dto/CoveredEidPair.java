/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.dto;

import java.util.List;

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
public class CoveredEidPair {

    private String srcEid;

    private String dstEid;

    private String srcDevice;

    private String srcPort;

    private String srcCna;

    private String dstCna;

    private String destDevice;

    private String destPort;

    private List<CoverageLink> coveredLinks;

    /**
     * Path type of this pair: {@link CoveragePathType#INTER_CHASSIS} (框间, 4
     * forward + 4 ACK links) or {@link CoveragePathType#INTRA_CHASSIS} (框内,
     * 2 forward + 2 ACK links). {@code null} for results of the original
     * interface.
     */
    private CoveragePathType type;
}
