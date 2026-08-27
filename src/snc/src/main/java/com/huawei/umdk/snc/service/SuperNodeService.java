/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: SNC (Supernode Network Controller) service
 * Create: 2026-07-07
 * Note:
 * History: 2026-07-07  Create File; 2026-07-16 key=value log format
 */
package com.huawei.umdk.snc.service;

import java.util.List;

import com.huawei.umdk.snc.log.Logger;
import com.huawei.umdk.snc.entity.DeviceEntity;
import com.huawei.umdk.snc.entity.NpuDevice;
import com.huawei.umdk.snc.entity.RoutePrefix;
import com.huawei.umdk.snc.entity.RoutingEntry;
import com.huawei.umdk.snc.entity.SuperNode;
import com.huawei.umdk.snc.entity.SwDevice;
import com.huawei.umdk.snc.store.SuperNodeStore;

public class SuperNodeService {

    private static final Logger LOG = new Logger(SuperNodeService.class);

    private final SuperNodeStore store;

    public SuperNodeService(SuperNodeStore store) {
        this.store = store;
    }

    public boolean importSuperNode(SuperNode superNode) {
        if (superNode == null) {
            LOG.error("importSuperNode: error=SuperNode must not be null");
            return false;
        }
        if (superNode.getName() == null || superNode.getName().isEmpty()) {
            LOG.error("importSuperNode: error=SuperNode name must not be null or empty");
            return false;
        }
        if (superNode.getAllDevices() == null || superNode.getAllDevices().isEmpty()) {
            LOG.error("importSuperNode: error=SuperNode devices must not be null or empty, name=%s", superNode.getName());
            return false;
        }
        LOG.info("importSuperNode: name=%s, npuDevices=%d, swDevices=%d",
            superNode.getName(),
            superNode.getNpuDevices() != null ? superNode.getNpuDevices().size() : 0,
            superNode.getSwDevices() != null ? superNode.getSwDevices().size() : 0);
        store.replace(superNode);
        return true;
    }

    public boolean addNpuDevices(String superNodeName, List<NpuDevice> devices) {
        if (superNodeName == null || superNodeName.isEmpty()) {
            LOG.error("addNpuDevices: error=superNodeName must not be null or empty");
            return false;
        }
        if (devices == null) {
            LOG.error("addNpuDevices: error=devices must not be null");
            return false;
        }
        LOG.info("addNpuDevices: superNode=%s, count=%d", superNodeName, devices.size());
        for (NpuDevice device : devices) {
            if (device == null) {
                LOG.error("addNpuDevices: error=device in list must not be null");
                return false;
            }
            LOG.debug("addNpuDevices: device=%s, superNode=%s", device.getDeviceName(), superNodeName);
            store.addNpuDevice(superNodeName, device);
        }
        return true;
    }

    public boolean addSwDevices(String superNodeName, List<SwDevice> devices) {
        if (superNodeName == null || superNodeName.isEmpty()) {
            LOG.error("addSwDevices: error=superNodeName must not be null or empty");
            return false;
        }
        if (devices == null) {
            LOG.error("addSwDevices: error=devices must not be null");
            return false;
        }
        LOG.info("addSwDevices: superNode=%s, count=%d", superNodeName, devices.size());
        for (SwDevice device : devices) {
            if (device == null) {
                LOG.error("addSwDevices: error=device in list must not be null");
                return false;
            }
            LOG.debug("addSwDevices: device=%s, superNode=%s", device.getDeviceName(), superNodeName);
            store.addSwDevice(superNodeName, device);
        }
        return true;
    }

    public boolean removeDevices(String superNodeName, List<String> deviceNames) {
        if (superNodeName == null || superNodeName.isEmpty()) {
            LOG.error("removeDevices: error=superNodeName must not be null or empty");
            return false;
        }
        if (deviceNames == null) {
            LOG.error("removeDevices: error=deviceNames must not be null");
            return false;
        }
        LOG.info("removeDevices: superNode=%s, count=%d", superNodeName, deviceNames.size());
        for (String deviceName : deviceNames) {
            if (deviceName == null || deviceName.isEmpty()) {
                LOG.error("removeDevices: error=deviceName in list must not be null or empty");
                return false;
            }
            LOG.debug("removeDevices: device=%s, superNode=%s", deviceName, superNodeName);
            store.removeDevice(superNodeName, deviceName);
        }
        return true;
    }

    public boolean addRoutingEntries(String superNodeName, String deviceName, Integer chipIndex,
                                  List<RoutingEntry> entries) {
        if (superNodeName == null || superNodeName.isEmpty()) {
            LOG.error("addRoutingEntries: error=superNodeName must not be null or empty");
            return false;
        }
        if (deviceName == null || deviceName.isEmpty()) {
            LOG.error("addRoutingEntries: error=deviceName must not be null or empty");
            return false;
        }
        if (chipIndex == null) {
            LOG.error("addRoutingEntries: error=chipIndex must not be null");
            return false;
        }
        if (entries == null) {
            LOG.error("addRoutingEntries: error=entries must not be null");
            return false;
        }
        LOG.info("addRoutingEntries: superNode=%s, device=%s, chip=%d, count=%d",
            superNodeName, deviceName, chipIndex, entries.size());
        for (RoutingEntry entry : entries) {
            if (entry == null || entry.getPrefix() == null) {
                LOG.error("addRoutingEntries: error=entry or entry.prefix in list must not be null");
                return false;
            }
            LOG.debug("addRoutingEntries: prefix=%s, superNode=%s, device=%s, chip=%d",
                entry.getPrefix(), superNodeName, deviceName, chipIndex);
            store.addRoutingEntry(superNodeName, deviceName, chipIndex, entry.getPrefix(), entry);
        }
        return true;
    }

    public boolean removeRoutingEntries(String superNodeName, String deviceName, Integer chipIndex,
                                     List<RoutePrefix> prefixes) {
        if (superNodeName == null || superNodeName.isEmpty()) {
            LOG.error("removeRoutingEntries: error=superNodeName must not be null or empty");
            return false;
        }
        if (deviceName == null || deviceName.isEmpty()) {
            LOG.error("removeRoutingEntries: error=deviceName must not be null or empty");
            return false;
        }
        if (chipIndex == null) {
            LOG.error("removeRoutingEntries: error=chipIndex must not be null");
            return false;
        }
        if (prefixes == null) {
            LOG.error("removeRoutingEntries: error=prefixes must not be null");
            return false;
        }
        LOG.info("removeRoutingEntries: superNode=%s, device=%s, chip=%d, count=%d",
            superNodeName, deviceName, chipIndex, prefixes.size());
        for (RoutePrefix prefix : prefixes) {
            if (prefix == null) {
                LOG.error("removeRoutingEntries: error=prefix in list must not be null");
                return false;
            }
            LOG.debug("removeRoutingEntries: prefix=%s, superNode=%s, device=%s, chip=%d",
                prefix, superNodeName, deviceName, chipIndex);
            store.removeRoutingEntry(superNodeName, deviceName, chipIndex, prefix);
        }
        return true;
    }

    public SuperNode getSuperNode(String name) {
        LOG.debug("getSuperNode: name=%s", name);
        return store.getSuperNode(name);
    }

    public void removeSuperNode(String name) {
        LOG.info("removeSuperNode: name=%s", name);
        store.removeSuperNode(name);
    }
}
