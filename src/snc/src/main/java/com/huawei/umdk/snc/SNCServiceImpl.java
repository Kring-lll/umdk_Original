/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc service impl module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc service impl module
 */
package com.huawei.umdk.snc;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import com.huawei.umdk.snc.exception.SNCException;
import com.huawei.umdk.snc.service.AclService;
import com.huawei.umdk.snc.service.PathService;
import com.huawei.umdk.snc.service.TopoService;
import com.huawei.umdk.snc.store.AclStore;
import com.huawei.umdk.snc.store.TopoStore;

import java.util.List;
import java.util.Set;

/**
 * SNC 服务实现
 *
 * 职责：
 * - 管理服务生命周期：init / uninit
 * - 维护自己的 TopoStore / AclStore，通过 TopoService / AclService 管理数据
 * - 委托 PathService 执行路径规划
 * - 对外暴露 Topo 和 ACL 的完整 CRUD 接口
 */
public class SNCServiceImpl implements SNCService {

    private SNCConfig config;
    private PathService pathService;

    // 数据存储（服务自身持有，独立于 PathService 的引擎数据）
    private final TopoStore topoStore;
    private final AclStore aclStore;
    private final TopoService topoService;
    private final AclService aclService;

    public SNCServiceImpl() {
        this(new SNCConfig());
    }

    public SNCServiceImpl(SNCConfig config) {
        this.config = config;
        this.topoStore = new TopoStore();
        this.aclStore = new AclStore();
        this.topoService = new TopoService(topoStore);
        this.aclService = new AclService(aclStore);
    }

    /** 设置 config（允许在 init 前更改配置） */
    public void setConfig(SNCConfig config) {
        this.config = config;
    }

    /** 初始化 - 创建 PathService 并调用 init，同时把 store 中已有的数据同步进去 */
    public void init() {
        pathService = new PathService(config);
        pathService.init();
        // 将 store 中已有数据同步到 pathService（兼容 init 前 setTopoData/setAclData 的场景）
        for (TopoData td : topoStore.getAll()) {
            pathService.setTopoData(td);
        }
        for (AclData ad : aclStore.getAll()) {
            pathService.setAclData(ad);
        }
    }

    // ========================================================================
    // Topo CRUD
    // ========================================================================

    /** 新增或更新拓扑（同步给 PathService 做路径计算；跳过 null/非法数据） */
    public void setTopoData(TopoData topoData) {
        if (topoData == null || topoData.getName() == null || topoData.getName().isEmpty()) {
            return;
        }
        // 无 devices 时绕过 TopoService 的校验，直接存入 store
        if (topoData.getDevices() == null || topoData.getDevices().isEmpty()) {
            topoStore.addOrUpdate(topoData);
        } else {
            topoService.addOrUpdateTopo(topoData);
        }
        if (pathService != null) {
            pathService.setTopoData(topoData);
        }
    }

    /** 根据名称查询拓扑（不存在时返回 null） */
    public TopoData getTopoData(String topoName) {
        try {
            return topoService.findTopo(topoName);
        } catch (SNCException e) {
            return null;
        }
    }

    /** 列出所有拓扑数据 */
    public List<TopoData> listAllTopos() {
        return topoService.listAllTopos();
    }

    /** 列出所有拓扑名称 */
    public Set<String> listAllTopoNames() {
        return topoService.listAllTopoNames();
    }

    /** 删除拓扑（不存在时静默成功） */
    public void removeTopoData(String topoName) {
        try {
            topoService.removeTopo(topoName);
        } catch (SNCException e) {
            // 不存在时静默成功
        }
    }

    /** 清空所有拓扑 */
    public void clearAllTopos() {
        topoService.clearAll();
    }

    // ========================================================================
    // ACL CRUD
    // ========================================================================

    /** 新增或更新 ACL 规则集（同步给 PathService 做路径计算；跳过 null/非法数据） */
    public void setAclData(AclData aclData) {
        if (aclData == null || aclData.getAclId() == null || aclData.getAclId().isEmpty()) {
            return;
        }
        aclService.addOrUpdateAcl(aclData);
        if (pathService != null) {
            pathService.setAclData(aclData);
        }
    }

    /** 根据 ID 查询 ACL（不存在时返回 null） */
    public AclData getAclData(String aclId) {
        try {
            return aclService.findAcl(aclId);
        } catch (SNCException e) {
            return null;
        }
    }

    /** 列出所有 ACL 数据 */
    public List<AclData> listAllAcls() {
        return aclService.listAllAcls();
    }

    /** 列出所有 ACL ID */
    public Set<String> listAllAclIds() {
        return aclService.listAllAclIds();
    }

    /** 删除 ACL 规则集（不存在时静默成功） */
    public void removeAclData(String aclId) {
        try {
            aclService.removeAcl(aclId);
        } catch (SNCException e) {
            // 不存在时静默成功
        }
    }

    /** 清空所有 ACL */
    public void clearAllAcls() {
        aclService.clearAll();
    }

    // ========================================================================
    // Plan
    // ========================================================================

    /** 路径规划 */
    public PathPlanResult planPath(PathPlanRequest request) {
        if (pathService == null) {
            return PathPlanResult.fail(PlanStatus.INVALID_PARAM, -99, "Service not initialized");
        }
        return pathService.planPath(request);
    }

    /** 反初始化 - 清理资源 */
    public void uninit() {
        if (pathService != null) {
            pathService.uninit();
            pathService = null;
        }
        topoStore.clear();
        aclStore.clear();
    }
}