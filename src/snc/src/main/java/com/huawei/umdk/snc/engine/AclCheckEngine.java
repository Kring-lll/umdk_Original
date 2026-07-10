/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SNC (Super Node Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.TpAclEntity;
import com.huawei.umdk.snc.entity.TransportType;

public class AclCheckEngine {

    public boolean checkBoth(AclData aclData, String srcEid, String dstEid,
                             String sourceCna, String destCna) {
        if (aclData == null || aclData.getTpAcls() == null) {
            return false;
        }

        AclKey forwardKey = new AclKey(srcEid, dstEid, TransportType.RCTP);
        TpAclEntity forwardEntity = aclData.getTpAcls().get(forwardKey);
        if (forwardEntity == null) {
            return false;
        }
        if (!sourceCna.equals(forwardEntity.getSourceCna())
            || !destCna.equals(forwardEntity.getDestCna())) {
            return false;
        }

        AclKey reverseKey = new AclKey(dstEid, srcEid, TransportType.RCTP);
        TpAclEntity reverseEntity = aclData.getTpAcls().get(reverseKey);
        if (reverseEntity == null) {
            return false;
        }
        if (!destCna.equals(reverseEntity.getSourceCna())
            || !sourceCna.equals(reverseEntity.getDestCna())) {
            return false;
        }

        return true;
    }
}
