package com.huawei.umdk.snc.store;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.ForwardingChip;
import com.huawei.umdk.snc.entity.RoutePrefix;
import com.huawei.umdk.snc.entity.RoutingEntry;
import com.huawei.umdk.snc.entity.RoutingTable;
import com.huawei.umdk.snc.entity.RoutingTableKey;
import com.huawei.umdk.snc.entity.SuperNode;

public class SuperNodeStore {

    private Map<String, SuperNode> superNodeMap;

    private Map<RoutingTableKey, RoutingTable> routingTableMap;

    public void init() {
        this.superNodeMap = new HashMap<>();
        this.routingTableMap = new HashMap<>();
    }

    public void replace(SuperNode superNode) {
        superNodeMap.put(superNode.getName(), superNode);
        if (superNode.getDevices() != null) {
            for (DeviceEntity device : superNode.getDevices().values()) {
                if (device.getForwardingChips() != null) {
                    for (ForwardingChip chip : device.getForwardingChips().values()) {
                        if (chip.getRoutingTable() != null) {
                            RoutingTableKey key = new RoutingTableKey(
                                superNode.getName(), device.getDeviceName(), chip.getChipIndex());
                            routingTableMap.put(key, chip.getRoutingTable());
                        }
                    }
                }
            }
        }
    }

    public void clear() {
        if (superNodeMap != null) {
            superNodeMap.clear();
        }
        if (routingTableMap != null) {
            routingTableMap.clear();
        }
    }

    public void removeSuperNode(String superNodeName) {
        if (superNodeMap != null) {
            superNodeMap.remove(superNodeName);
        }
        if (routingTableMap != null) {
            routingTableMap.entrySet().removeIf(e -> e.getKey().getSuperNodeName().equals(superNodeName));
        }
    }

    public SuperNode getSuperNode(String name) {
        return superNodeMap.get(name);
    }

    public RoutingTable getRoutingTable(RoutingTableKey key) {
        return routingTableMap.get(key);
    }

    public void addDevice(String superNodeName, DeviceEntity device) {
        SuperNode superNode = superNodeMap.get(superNodeName);
        if (superNode == null) {
            superNode = new SuperNode();
            superNode.setName(superNodeName);
            superNode.setDevices(new HashMap<>());
            superNodeMap.put(superNodeName, superNode);
        }
        if (superNode.getDevices() == null) {
            superNode.setDevices(new HashMap<>());
        }
        superNode.getDevices().put(device.getDeviceName(), device);
        if (device.getForwardingChips() != null) {
            for (ForwardingChip chip : device.getForwardingChips().values()) {
                if (chip.getRoutingTable() != null) {
                    RoutingTableKey key = new RoutingTableKey(superNodeName, device.getDeviceName(), chip.getChipIndex());
                    routingTableMap.put(key, chip.getRoutingTable());
                }
            }
        }
    }

    public void removeDevice(String superNodeName, String deviceName) {
        if (superNodeMap != null) {
            SuperNode superNode = superNodeMap.get(superNodeName);
            if (superNode != null && superNode.getDevices() != null) {
                superNode.getDevices().remove(deviceName);
            }
        }
        if (routingTableMap != null) {
            routingTableMap.entrySet().removeIf(e ->
                e.getKey().getSuperNodeName().equals(superNodeName)
                    && e.getKey().getDeviceName().equals(deviceName));
        }
    }

    public void addRoutingEntry(String superNodeName, String deviceName, Integer chipIndex,
                                RoutePrefix prefix, RoutingEntry entry) {
        RoutingTableKey key = new RoutingTableKey(superNodeName, deviceName, chipIndex);
        RoutingTable rt = routingTableMap.get(key);
        if (rt != null && rt.getRoutes() != null) {
            rt.getRoutes().put(prefix, entry);
            updateMaskLengths(rt);
        }
    }

    public void removeRoutingEntry(String superNodeName, String deviceName, Integer chipIndex,
                                   RoutePrefix prefix) {
        RoutingTableKey key = new RoutingTableKey(superNodeName, deviceName, chipIndex);
        RoutingTable rt = routingTableMap.get(key);
        if (rt != null && rt.getRoutes() != null) {
            rt.getRoutes().remove(prefix);
            updateMaskLengths(rt);
        }
    }

    private void updateMaskLengths(RoutingTable rt) {
        List<Integer> masks = rt.getRoutes().keySet().stream()
            .map(RoutePrefix::getMaskLength)
            .sorted(Comparator.reverseOrder())
            .collect(Collectors.toList());
        rt.setMaskLengths(masks);
    }
}
