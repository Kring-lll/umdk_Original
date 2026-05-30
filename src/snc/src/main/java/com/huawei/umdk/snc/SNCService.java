/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc service module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc service module
 */
package com.huawei.umdk.snc;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.PathPlanRequest;
import com.huawei.umdk.snc.entity.PathPlanResult;
import com.huawei.umdk.snc.entity.TopoData;

import java.util.List;
import java.util.Set;

/**
 * SNC 对外服务接口
 * 
 * 职责：
 * - 管理服务生命周期：init / uninit
 * - 拓扑数据 CRUD：setTopoData / getTopoData / listAllTopos / removeTopoData
 * - ACL 数据 CRUD：setAclData / getAclData / listAllAcls / removeAclData
 * - 路径查询：planPath
 */
public interface SNCService {

    /** 初始化服务 */
    void init();

    // ====== Topo CRUD ======

    /** 新增或更新拓扑 */
    void setTopoData(TopoData topoData);

    /** 根据名称查询拓扑 */
    TopoData getTopoData(String topoName);

    /** 列出所有拓扑数据 */
    List<TopoData> listAllTopos();

    /** 列出所有拓扑名称 */
    Set<String> listAllTopoNames();

    /** 删除拓扑 */
    void removeTopoData(String topoName);

    /** 清空所有拓扑 */
    void clearAllTopos();

    // ====== ACL CRUD ======

    /** 新增或更新 ACL 规则集 */
    void setAclData(AclData aclData);

    /** 根据 ID 查询 ACL */
    AclData getAclData(String aclId);

    /** 列出所有 ACL 数据 */
    List<AclData> listAllAcls();

    /** 列出所有 ACL ID */
    Set<String> listAllAclIds();

    /** 删除 ACL 规则集 */
    void removeAclData(String aclId);

    /** 清空所有 ACL */
    void clearAllAcls();

    // ====== Plan ======

    /** 执行路径查询 */
    PathPlanResult planPath(PathPlanRequest request);

    /** 反初始化 / 资源释放 */
    void uninit();
}
