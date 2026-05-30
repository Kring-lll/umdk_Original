package com.huawei.umdk.snc.service;

import com.huawei.umdk.snc.entity.AclData;
import com.huawei.umdk.snc.entity.TpAclEntity;
import com.huawei.umdk.snc.exception.SNCException;
import com.huawei.umdk.snc.entity.PlanStatus;
import com.huawei.umdk.snc.store.AclStore;
import com.huawei.umdk.snc.util.CNAUtils;
import com.huawei.umdk.snc.util.EIDUtils;

import java.util.*;

/**
 * ACL数据管理服务
 *
 * 职责：
 * - ACL规则集的增加、查询、删除
 * - 参数校验
 */
public class AclService {

    private final AclStore aclStore;

    public AclService(AclStore aclStore) {
        this.aclStore = aclStore;
    }

    /**
     * 新增或更新 ACL 规则集
     */
    public void addOrUpdateAcl(AclData aclData) {
        validateAclData(aclData);
        aclStore.addOrUpdate(aclData);
    }

    /**
     * 查询 ACL 规则集
     */
    public AclData findAcl(String aclId) {
        if (aclId == null || aclId.isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "aclId must not be null or empty");
        }
        AclData aclData = aclStore.find(aclId);
        if (aclData == null) {
            throw new SNCException(PlanStatus.ACL_NOT_FOUND, "ACL not found: " + aclId);
        }
        return aclData;
    }

    /**
     * 删除 ACL 规则集
     */
    public void removeAcl(String aclId) {
        if (aclId == null || aclId.isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "aclId must not be null or empty");
        }
        AclData removed = aclStore.remove(aclId);
        if (removed == null) {
            throw new SNCException(PlanStatus.ACL_NOT_FOUND, "ACL not found: " + aclId);
        }
    }

    /**
     * 判断 ACL 规则集是否存在
     */
    public boolean containsAcl(String aclId) {
        return aclId != null && aclStore.contains(aclId);
    }

    /**
     * 获取已加载的 ACL 规则集数量
     */
    public int getAclCount() {
        return aclStore.size();
    }

    /**
     * 清空所有 ACL 规则集
     */
    public void clearAll() {
        aclStore.clear();
    }

    /**
     * 列出所有 ACL 数据
     */
    public List<AclData> listAllAcls() {
        return new ArrayList<>(aclStore.getAll());
    }

    /**
     * 列出所有 ACL ID
     */
    public Set<String> listAllAclIds() {
        return aclStore.getIds();
    }

    private void validateAclData(AclData aclData) {
        if (aclData == null) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "AclData must not be null");
        }
        if (aclData.getAclId() == null || aclData.getAclId().isEmpty()) {
            throw new SNCException(PlanStatus.INVALID_PARAM, "AclData aclId must not be null or empty");
        }
        // 校验每个 TpAclEntity 的 EID 和 CNA 格式
        Map<?, TpAclEntity> tpAcls = aclData.getTpAcls();
        if (tpAcls != null) {
            for (TpAclEntity rule : tpAcls.values()) {
                if (rule == null) {
                    continue;
                }
                if (rule.getSourceEid() == null || !EIDUtils.isValid(rule.getSourceEid())) {
                    throw new SNCException(PlanStatus.INVALID_PARAM,
                            "TpAclEntity sourceEid invalid (need 32 hex chars): " + rule.getSourceEid());
                }
                if (rule.getDestEid() == null || !EIDUtils.isValid(rule.getDestEid())) {
                    throw new SNCException(PlanStatus.INVALID_PARAM,
                            "TpAclEntity destEid invalid (need 32 hex chars): " + rule.getDestEid());
                }
                if (rule.getSourceCna() == null || !CNAUtils.isValid(rule.getSourceCna())) {
                    throw new SNCException(PlanStatus.INVALID_PARAM,
                            "TpAclEntity sourceCna invalid (need 24-bit 0x hex): " + rule.getSourceCna());
                }
                if (rule.getDestCna() == null || !CNAUtils.isValid(rule.getDestCna())) {
                    throw new SNCException(PlanStatus.INVALID_PARAM,
                            "TpAclEntity destCna invalid (need 24-bit 0x hex): " + rule.getDestCna());
                }
            }
        }
    }
}