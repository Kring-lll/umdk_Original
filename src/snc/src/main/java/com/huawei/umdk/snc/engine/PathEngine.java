/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: snc pathengine module
 * Create: 2026-05-29
 * Note:
 * History: 2026-05-29 create snc pathengine module
 */
package com.huawei.umdk.snc.engine;

import com.huawei.umdk.snc.config.SNCConfig;
import com.huawei.umdk.snc.entity.*;
import com.huawei.umdk.snc.util.CNAUtils;
import com.huawei.umdk.snc.util.EIDUtils;

import java.util.*;

/**
 * 路径规划引擎 -- 路径还原
 *
 * 职责：
 * - 根据 PathPlanRequest（device+port 模式）在 TopoData 中还原路径
 * - 产生 PathPlanResult（包含 List<PathInfo>，每个 PathInfo 含 List<HopInfo>）
 * 
 * 注意：拓扑和 ACL 数据由 PathService 管理。
 */
public class PathEngine {

    private final SNCConfig config;
    private final RouteLookupEngine routeLookupEngine;

    public PathEngine(SNCConfig config) {
        this.config = config;
        this.routeLookupEngine = new RouteLookupEngine();
    }

    /**
     * 路径还原
     */
    public PathPlanResult resolvePath(TopoData topoData, PathPlanRequest request) {
        // 1. 参数校验
        String err = validateRequest(request);
        if (err != null) {
            return PathPlanResult.fail(PlanStatus.INVALID_PARAM, -1, err);
        }

        // 2. 查找设备
        DeviceEntity srcDevice = topoData.findDevice(request.getSrcDevice());
        DeviceEntity destDevice = topoData.findDevice(request.getDestDevice());
        if (srcDevice == null) {
            return PathPlanResult.fail(PlanStatus.SRC_EID_NOT_FOUND, -3,
                    "Src device not found: " + request.getSrcDevice());
        }
        if (destDevice == null) {
            return PathPlanResult.fail(PlanStatus.DEST_EID_NOT_FOUND, -4,
                    "Dest device not found: " + request.getDestDevice());
        }

        // 3. 源目相同
        if (request.getSrcDevice().equals(request.getDestDevice())) {
            return PathPlanResult.fail(PlanStatus.INVALID_PARAM, -5,
                    "Source and destination device are the same");
        }

        // 4. 构建有序设备列表: [srcDevice, ...interDevices..., destDevice]
        List<DeviceEntity> orderedDevices = buildOrderedDeviceList(topoData, request);
        if (orderedDevices == null) {
            return PathPlanResult.fail(PlanStatus.INVALID_PARAM, -6,
                    "Invalid interDevices configuration");
        }

        // 构建每跳的指定出端口 map（deviceId → outPort）
        Map<String, String> specifiedOutPorts = buildOutPortMap(request);

        // 5. 逐跳还原
        List<HopInfo> hopInfos = new ArrayList<>();

        for (int i = 0; i < orderedDevices.size(); i++) {
            DeviceEntity currentDevice = orderedDevices.get(i);
            boolean isLastDevice = (i == orderedDevices.size() - 1);

            String inPort = null;
            String outPort = null;

            if (isLastDevice) {
                // 最后一个设备（目的端）：outPort=null, inPort 从上一跳推导
                outPort = null;
                if (i > 0) {
                    inPort = findInPort(currentDevice, orderedDevices.get(i - 1).getDeviceId());
                }
                hopInfos.add(new HopInfo(currentDevice.getDeviceId(),
                        inPort,
                        outPort,
                        currentDevice.getDeviceType()));
                break;
            }

            // 非最后设备：需要找到去往下一台设备的出端口
            DeviceEntity nextDevice = orderedDevices.get(i + 1);

            // 入端口：第一个设备无入端口，其他设备的入端口从拓扑推导
            if (i > 0) {
                inPort = findInPort(currentDevice, orderedDevices.get(i - 1).getDeviceId());
            }

            // 出端口：优先使用 interDevices 中指定的出端口，否则从路由表查找
            String specifiedOut = specifiedOutPorts.get(currentDevice.getDeviceId());
            if (specifiedOut != null && !specifiedOut.isEmpty()) {
                outPort = specifiedOut;
            } else {
                outPort = resolveOutPort(currentDevice, nextDevice, destDevice, topoData);
            }

            if (outPort == null || outPort.isEmpty()) {
                return PathPlanResult.fail(PlanStatus.PATH_NOT_FOUND, -7,
                        "Cannot resolve outPort from " + currentDevice.getDeviceId()
                                + " to " + nextDevice.getDeviceId());
            }

            hopInfos.add(new HopInfo(currentDevice.getDeviceId(),
                    inPort,
                    outPort,
                    currentDevice.getDeviceType()));
        }

        // 6. 阶段5：构造输出（设计文档 8.6）
        PathInfo pathInfo = new PathInfo();
        pathInfo.setHops(hopInfos);

        PathPlanResult result = new PathPlanResult();
        result.setStatus(PlanStatus.SUCCESS);
        result.setErrorCode(0);
        result.setPaths(Collections.singletonList(pathInfo));

        // 设置源/目的 EID
        result.setSourceEid(findDeviceEid(srcDevice));
        result.setDestEid(findDeviceEid(destDevice));

        // 8.6 阶段5：构造输出 — 以下字段暂时打桩（待后续实现）
        // multiPath / spray / ackUdpSrcPort / dataUdpSrcPort 暂不填充
        // 原因：这些字段依赖 URC/散列/多路径算法，当前版本尚未实现

        return result;
    }

    /**
     * 参数校验
     */
    private String validateRequest(PathPlanRequest request) {
        if (request == null) {
            return "PathPlanRequest must not be null";
        }
        if (request.getSrcDevice() == null || request.getSrcDevice().isEmpty()) {
            return "srcDevice must not be null or empty";
        }
        if (request.getSrcPort() == null || request.getSrcPort().isEmpty()) {
            return "srcPort must not be null or empty";
        }
        if (request.getDestDevice() == null || request.getDestDevice().isEmpty()) {
            return "destDevice must not be null or empty";
        }
        if (request.getDestPort() == null || request.getDestPort().isEmpty()) {
            return "destPort must not be null or empty";
        }
        return null;
    }

    /**
     * 构建有序设备列表：[srcDevice, interDevice1, interDevice2, ..., destDevice]
     */
    private List<DeviceEntity> buildOrderedDeviceList(TopoData topoData, PathPlanRequest request) {
        List<DeviceEntity> devices = new ArrayList<>();

        // 源设备
        DeviceEntity src = topoData.findDevice(request.getSrcDevice());
        if (src == null) return null;
        devices.add(src);

        // 中间设备（按 interDevices 顺序）
        LinkedHashMap<String, String> interDevices = request.getInterDevices();
        if (interDevices != null) {
            for (Map.Entry<String, String> entry : interDevices.entrySet()) {
                DeviceEntity dev = topoData.findDevice(entry.getKey());
                if (dev == null) return null;
                devices.add(dev);
            }
        }

        // 目的设备
        DeviceEntity dest = topoData.findDevice(request.getDestDevice());
        if (dest == null) return null;
        devices.add(dest);

        return devices;
    }

    /**
     * 构建指定出端口映射：deviceId → outPort
     * 包含 srcDevice（使用 srcPort 作为 outPort）和 interDevices
     */
    private Map<String, String> buildOutPortMap(PathPlanRequest request) {
        LinkedHashMap<String, String> map = new LinkedHashMap<>();
        // srcDevice 的出端口即 srcPort
        if (request.getSrcDevice() != null && request.getSrcPort() != null) {
            map.put(request.getSrcDevice(), request.getSrcPort());
        }
        // interDevices 的指定出端口
        if (request.getInterDevices() != null) {
            map.putAll(request.getInterDevices());
        }
        return map;
    }

    /**
     * 通过路由表查找当前设备去往下一台设备的出端口
     *
     * 路由查找逻辑（设计文档 §10.5 Step 10）：
     * 使用目的设备的端口 CNA（24 位）转换为 32 位路由前缀
     * 再与当前设备的路由表进行最长前缀匹配
     */
    private String resolveOutPort(DeviceEntity currentDevice, DeviceEntity nextDevice,
                                   DeviceEntity destDevice, TopoData topoData) {
        if (currentDevice == null || nextDevice == null || destDevice == null) return null;

        // Step 10: 使用目的设备端口 CNA 处理后匹配路由表（设计文档 §10.5）
        // 24 位 CNA → 32 位前缀 → 路由表 LPM 查找
        String destCna = findDeviceCna(destDevice);
        if (destCna == null) return null;

        // CNA 转换为路由前缀（24 位 → 32 位）
        String routePrefix = CNAUtils.cnaToRoutePrefix(destCna);
        if (routePrefix == null) return null;

        // 查找路由表
        RouterTable routerTable = currentDevice.findRouterTable(0);
        if (routerTable == null) {
            List<RouterTable> rts = currentDevice.getRouterTables();
            routerTable = (rts != null && !rts.isEmpty()) ? rts.get(0) : null;
        }
        if (routerTable == null) return null;

        // 使用 CNA 转换的路由前缀进行最长前缀匹配
        RouteEntry route = routeLookupEngine.lookup(routerTable, routePrefix);
        if (route == null) {
            route = routeLookupEngine.lookupDefault(routerTable);
        }
        if (route == null) return null;

        // 取第一个 OutPortInfo 的出端口
        if (route.getOutPortInfos() != null && !route.getOutPortInfos().isEmpty()) {
            OutPortInfo firstHop = route.getOutPortInfos().get(0);
            String outInterface = firstHop.getOutInterface();
            if (outInterface != null && !outInterface.isEmpty()) {
                return outInterface;
            }
        }

        // Fallback: 通过拓扑连接找直接相连的出端口
        if (currentDevice.getPorts() != null) {
            for (PortEntity port : currentDevice.getPorts()) {
                if (port.getConnectedDevice() != null
                        && port.getConnectedDevice().equals(nextDevice.getDeviceId())) {
                    return port.getName();
                }
            }
        }

        return null;
    }

    /**
     * 查找设备的端口 CNA（用于路由前缀计算）
     * 取第一个有效端口的 CNA（同一设备上所有端口共享相同的设备 CNA 高位）
     */
    private String findDeviceCna(DeviceEntity device) {
        if (device == null) return null;
        if (device.getPorts() != null) {
            for (PortEntity port : device.getPorts()) {
                if (port.getCna() != null && !port.getCna().isEmpty()) {
                    return port.getCna();
                }
            }
        }
        return null;
    }

    /**
     * 查找设备上某个端口的 EID（用作路由 key）
     * 取第一个有效端口的 EID
     */
    private String findDeviceEid(DeviceEntity device) {
        if (device == null) return null;
        // 物理端口 EID
        if (device.getPorts() != null) {
            for (PortEntity port : device.getPorts()) {
                if (port.getEid() != null && !port.getEid().isEmpty()) {
                    return port.getEid();
                }
            }
        }
        // 逻辑端口 EID
        if (device.getLogicPorts() != null) {
            for (LogicPortEntity lp : device.getLogicPorts()) {
                if (lp.getEid() != null && !lp.getEid().isEmpty()) {
                    return lp.getEid();
                }
            }
        }
        return null;
    }

    /**
     * 查找下一跳设备上连接到指定设备的入端口
     */
    private String findInPort(DeviceEntity nextDevice, String prevDeviceId) {
        if (nextDevice == null) return null;
        if (nextDevice.getPorts() != null) {
            for (PortEntity port : nextDevice.getPorts()) {
                if (port.getConnectedDevice() != null && port.getConnectedDevice().equals(prevDeviceId)) {
                    return port.getName();
                }
            }
        }
        return null;
    }
}