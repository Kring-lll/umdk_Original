/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: SNC (Super Node Controller) service
 * Author: OpenCode
 * Create: 2024-11-04
 * Note:
 * History: 2024-11-04  Create File
 */
package com.huawei.umdk.snc;

import java.util.List;
import java.util.Map;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.dto.PathPlanRequest;
import com.huawei.umdk.snc.dto.PathPlanResult;
import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.AclKey;
import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.RoutePrefix;
import com.huawei.umdk.snc.entity.RoutingEntry;
import com.huawei.umdk.snc.entity.SuperNode;
import com.huawei.umdk.snc.entity.TpAclEntity;

public interface SNCService {

    void init(SNCConfig config);

    void uninit();

    void setSuperNode(SuperNode superNode);

    void addDevices(String superNodeName, List<DeviceEntity> devices);

    void removeDevices(String superNodeName, List<String> deviceNames);

    void addRoutingEntries(String superNodeName, String deviceName, Integer chipIndex,
                           List<RoutingEntry> entries);

    void removeRoutingEntries(String superNodeName, String deviceName, Integer chipIndex,
                              List<RoutePrefix> prefixes);

    void setAclData(AclData aclData);

    void addAclRules(String superNodeName, Map<AclKey, TpAclEntity> rules);

    void removeAclRules(String superNodeName, List<AclKey> keys);

    SuperNode getSuperNode(String name);

    void removeSuperNode(String name);

    AclData getAclData(String superNodeName);

    void removeAclData(String superNodeName);

    PathPlanResult planPath(PathPlanRequest request);
}
