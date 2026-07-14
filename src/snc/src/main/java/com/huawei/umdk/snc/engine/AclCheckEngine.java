/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File
 */
package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.log.Logger;
import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.TpAclEntity;
import com.huawei.umdk.snc.entity.TransportType;

public class AclCheckEngine {

    private static final Logger LOG = new Logger(AclCheckEngine.class);

    public boolean checkBothDirection(AclData aclData, String srcEid, String dstEid,
                             String sourceCna, String destCna) {
        LOG.debug("checkBothDirection: srcEid=%s, dstEid=%s, srcCna=%s, dstCna=%s",
            srcEid, dstEid, sourceCna, destCna);
        if (aclData == null || aclData.getTpAcls() == null) {
            LOG.warn("checkBothDirection: aclData or tpAcls is null");
            return false;
        }
        if (srcEid == null || dstEid == null || sourceCna == null || destCna == null) {
            LOG.warn("checkBothDirection: one or more parameters are null");
            return false;
        }

        AclKey forwardKey = new AclKey(srcEid, dstEid, TransportType.RCTP);
        TpAclEntity forwardEntity = aclData.getTpAcls().get(forwardKey);
        if (forwardEntity == null) {
            LOG.warn("checkBothDirection: forward ACL entity not found for key=%s", forwardKey);
            return false;
        }
        if (!sourceCna.equals(forwardEntity.getSourceCna())
            || !destCna.equals(forwardEntity.getDestCna())) {
            LOG.warn("checkBothDirection: forward ACL CNA mismatch, expectedSrcCna=%s, expectedDstCna=%s, gotSrcCna=%s, gotDstCna=%s",
                sourceCna, destCna, forwardEntity.getSourceCna(), forwardEntity.getDestCna());
            return false;
        }

        AclKey reverseKey = new AclKey(dstEid, srcEid, TransportType.RCTP);
        TpAclEntity reverseEntity = aclData.getTpAcls().get(reverseKey);
        if (reverseEntity == null) {
            LOG.warn("checkBothDirection: reverse ACL entity not found for key=%s", reverseKey);
            return false;
        }
        if (!destCna.equals(reverseEntity.getSourceCna())
            || !sourceCna.equals(reverseEntity.getDestCna())) {
            LOG.warn("checkBothDirection: reverse ACL CNA mismatch, expectedSrcCna=%s, expectedDstCna=%s, gotSrcCna=%s, gotDstCna=%s",
                destCna, sourceCna, reverseEntity.getSourceCna(), reverseEntity.getDestCna());
            return false;
        }

        LOG.debug("checkBothDirection: ACL bidirectional check passed");
        return true;
    }
}
