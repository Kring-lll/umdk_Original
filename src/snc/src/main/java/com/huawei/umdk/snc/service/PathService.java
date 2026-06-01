/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc path service module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc path service module
 */
package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.engine.AclCheckEngine;
import com.huawei.umdk.snc.engine.PathEngine;
import com.huawei.umdk.snc.engine.RouteLookupEngine;
import com.huawei.umdk.snc.entity.*;

/**
 * 路径规划服务
 * 
 * 职责：
 * - 对外提供统一的路径规划入口
 * - 管理引擎层的 API 生命周期：init / setTopoData / setAclData / planPath / uninit
 * - 负责引擎按序调用：路径还原 -> ACL 校验 -> 结果组装
 */
public class PathService {

    private final SNCConfig config;

    private PathEngine pathEngine;
    private AclCheckEngine aclCheckEngine;

    private TopoData topoData;
    private AclData aclData;

    public PathService(SNCConfig config) {
        this.config = config;
    }

    /**
     * 初始化引擎
     */
    public void init() {
        this.pathEngine = new PathEngine(config);
        this.aclCheckEngine = new AclCheckEngine();
    }

    /**
     * 设置拓扑数据
     */
    public void setTopoData(TopoData topoData) {
        this.topoData = topoData;
    }

    /**
     * 设置 ACL 数据
     */
    public void setAclData(AclData aclData) {
        this.aclData = aclData;
    }

    /**
     * 反初始化 / 资源释放
     */
    public void uninit() {
        this.topoData = null;
        this.aclData = null;
        this.pathEngine = null;
        this.aclCheckEngine = null;
    }

    /**
     * 执行路径规划 -- 主入口
     *
     * 流程：路径还原 -> ACL 校验 -> 结果组装
     */
    public PathPlanResult planPath(PathPlanRequest request) {
        if (pathEngine == null) {
            return PathPlanResult.fail(PlanStatus.INVALID_PARAM, -1, "PathEngine not initialized");
        }
        if (topoData == null) {
            return PathPlanResult.fail(PlanStatus.TOPO_NOT_FOUND, -2, "TopoData not set");
        }

        // 1. 路径还原
        PathPlanResult result = pathEngine.resolvePath(topoData, request);
        if (result.getStatus() != PlanStatus.SUCCESS) {
            return result;
        }

        // 2. ACL 校验
        if (aclCheckEngine != null && aclData != null && aclData.getTpAcls() != null
                && !aclData.getTpAcls().isEmpty()) {
            result = aclCheckEngine.checkPath(aclData, result, topoData);
        }

        return result;
    }
}