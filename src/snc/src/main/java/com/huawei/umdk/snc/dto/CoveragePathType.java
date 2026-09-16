/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-09-13
 * Note:
 * History: 2026-09-13  Create File
 */
package com.huawei.umdk.snc.dto;

/**
 * Type of the path used by one EID pair of the extended coverage planning.
 *
 * <p>The extended planning runs in two phases:
 * <ol>
 *   <li>{@link #INTER_CHASSIS} — inter-chassis (框间) paths
 *       {@code NPU -> L1SW -> L2SW -> L1SW -> NPU}, which fully cover the
 *       L1SW↔L2SW segment;</li>
 *   <li>{@link #INTRA_CHASSIS} — intra-chassis (框内) paths
 *       {@code NPU -> L1SW -> NPU}, used afterwards to cover the NPU↔L1SW
 *       out-ports that the inter-chassis phase left uncovered.</li>
 * </ol>
 */
public enum CoveragePathType {
    /** Inter-chassis path (traverses the L2SW spine). */
    INTER_CHASSIS,

    /** Intra-chassis path (stays inside one chassis: NPU → L1SW → NPU). */
    INTRA_CHASSIS
}
