package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.entity.*;

import java.util.List;
import java.util.Map;

/**
 * ACL 校验引擎
 * 
 * 职责：
 * - 基于 (sourceEid, destEid, transportType) 查找 ACL 规则
 * - 校验源/目的设备的端口 CNA 是否与 ACL 规则匹配
 * - 将校验结果通过 PathPlanResult.status 返回（与路由查找平行）
 */
public class AclCheckEngine {

    /**
     * 对路径查询结果进行 ACL 校验（基于源/目的 EID + transportType 查找规则表）
     * 
     * @param aclData  ACL 数据
     * @param result   路径查询结果
     * @param topoData 拓扑数据（用于查找端口 CNA）
     * @return 如果 ACL 校验失败，result.status 会被设为对应的错误码
     */
    public PathPlanResult checkPath(AclData aclData, PathPlanResult result, TopoData topoData) {
        if (aclData == null || result == null || topoData == null) {
            return result;
        }
        // 如果已经失败了就不再做 ACL 检查
        if (result.getStatus() != null && result.getStatus() != PlanStatus.SUCCESS) {
            return result;
        }
        Map<AclKey, TpAclEntity> tpAcls = aclData.getTpAcls();
        if (tpAcls == null || tpAcls.isEmpty()) {
            return result;
        }

        String srcEid = result.getSourceEid();
        String dstEid = result.getDestEid();
        if (srcEid == null || dstEid == null) {
            return result;
        }

        // 用 (sourceEid, destEid) 查找 ACL 规则（transportType 从拓扑获取，这里先做通用匹配）
        boolean aclFound = false;
        boolean cnaMatched = false;

        for (Map.Entry<AclKey, TpAclEntity> entry : tpAcls.entrySet()) {
            TpAclEntity rule = entry.getValue();
            if (rule == null) continue;

            // 通过 EID 匹配
            if (!srcEid.equals(rule.getSourceEid()) || !dstEid.equals(rule.getDestEid())) {
                continue;
            }
            aclFound = true;

            // 找到 ACL 规则后，在拓扑中查找源/目的设备的端口 CNA，校验是否匹配
            String srcCna = findSrcPortCna(topoData, result);
            String dstCna = findDstPortCna(topoData, result);

            if (srcCna != null && dstCna != null
                    && srcCna.equalsIgnoreCase(rule.getSourceCna())
                    && dstCna.equalsIgnoreCase(rule.getDestCna())) {
                cnaMatched = true;
                break;
            }
        }

        if (!aclFound) {
            result.setStatus(PlanStatus.ACL_NOT_FOUND);
            result.setErrorCode(PlanStatus.ACL_NOT_FOUND.getCode());
            result.setErrorMessage("ACL rule not found for sourceEid=" + srcEid + ", destEid=" + dstEid);
        } else if (!cnaMatched) {
            result.setStatus(PlanStatus.ACL_DENIED);
            result.setErrorCode(PlanStatus.ACL_DENIED.getCode());
            result.setErrorMessage("ACL CNA mismatch for sourceEid=" + srcEid + ", destEid=" + dstEid);
        }

        return result;
    }

    /**
     * 从拓扑中查找源设备端口的 CNA
     */
    private String findSrcPortCna(TopoData topoData, PathPlanResult result) {
        List<PathInfo> paths = result.getPaths();
        if (paths == null || paths.isEmpty()) return null;
        PathInfo firstPath = paths.get(0);
        List<HopInfo> hops = firstPath.getHops();
        if (hops == null || hops.isEmpty()) return null;
        HopInfo first = hops.get(0);
        return findPortCna(topoData, first.getDeviceId(), first.getOutPort());
    }

    /**
     * 从拓扑中查找目的设备端口的 CNA
     */
    private String findDstPortCna(TopoData topoData, PathPlanResult result) {
        List<PathInfo> paths = result.getPaths();
        if (paths == null || paths.isEmpty()) return null;
        PathInfo firstPath = paths.get(0);
        List<HopInfo> hops = firstPath.getHops();
        if (hops == null || hops.isEmpty()) return null;
        HopInfo last = hops.get(hops.size() - 1);
        String inPort = last.getInPort();
        String portName = (inPort != null && !inPort.isEmpty()) ? inPort : last.getOutPort();
        return findPortCna(topoData, last.getDeviceId(), portName);
    }

    /**
     * 在拓扑中查找指定设备端口的 CNA 值
     */
    private String findPortCna(TopoData topoData, String deviceId, String portName) {
        if (topoData == null || deviceId == null || portName == null || portName.isEmpty()) return null;
        DeviceEntity device = topoData.findDevice(deviceId);
        if (device == null) return null;

        // 先查物理端口
        if (device.getPorts() != null) {
            for (PortEntity p : device.getPorts()) {
                if (p != null && portName.equals(p.getName())) {
                    return p.getCna();
                }
            }
        }
        // 再尝试逻辑端口
        if (device.getLogicPorts() != null && device.getPorts() != null) {
            for (LogicPortEntity lp : device.getLogicPorts()) {
                if (lp != null && portName.equals(lp.getName())) {
                    List<String> phys = lp.getPorts();
                    if (phys != null) {
                        for (String physName : phys) {
                            for (PortEntity p : device.getPorts()) {
                                if (p != null && physName.equals(p.getName())) {
                                    return p.getCna();
                                }
                            }
                        }
                    }
                }
            }
        }
        return null;
    }
}
