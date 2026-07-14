/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Author: guoshuangjian
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.config.Logger;
import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.TpAclEntity;
import com.huawei.umdk.snc.entity.TransportType;

public class AclCheckEngine {

    private static final Logger LOG = new Logger(AclCheckEngine.class);

    public boolean checkBothDirection(AclData aclData, String srcEid, String dstEid,
                             String sourceCna, String destCna) {
        if (aclData == null || aclData.getTpAcls() == null) {
            LOG.warn("checkBothDirection: aclData or tpAcls is null");
            return false;
        }
        if (srcEid == null || dstEid == null || sourceCna == null || destCna == null) {
            LOG.warn("checkBothDirection: null parameter, srcEid=%s, dstEid=%s", srcEid, dstEid);
            return false;
        }

        AclKey forwardKey = new AclKey(srcEid, dstEid, TransportType.RCTP);
        TpAclEntity forwardEntity = aclData.getTpAcls().get(forwardKey);
        if (forwardEntity == null) {
            LOG.warn("forward ACL not found: %s -> %s", srcEid, dstEid);
            return false;
        }
        if (!sourceCna.equals(forwardEntity.getSourceCna())
            || !destCna.equals(forwardEntity.getDestCna())) {
            LOG.warn("forward ACL CNA mismatch: expected srcCna=%s dstCna=%s, got srcCna=%s dstCna=%s",
                sourceCna, destCna, forwardEntity.getSourceCna(), forwardEntity.getDestCna());
            return false;
        }

        AclKey reverseKey = new AclKey(dstEid, srcEid, TransportType.RCTP);
        TpAclEntity reverseEntity = aclData.getTpAcls().get(reverseKey);
        if (reverseEntity == null) {
            LOG.warn("reverse ACL not found: %s -> %s", dstEid, srcEid);
            return false;
        }
        if (!destCna.equals(reverseEntity.getSourceCna())
            || !sourceCna.equals(reverseEntity.getDestCna())) {
            LOG.warn("reverse ACL CNA mismatch: expected srcCna=%s dstCna=%s, got srcCna=%s dstCna=%s",
                destCna, sourceCna, reverseEntity.getSourceCna(), reverseEntity.getDestCna());
            return false;
        }

        LOG.debug("ACL check passed: %s <-> %s", srcEid, dstEid);
        return true;
    }
}
