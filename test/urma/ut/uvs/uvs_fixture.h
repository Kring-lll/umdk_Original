/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UVS unit test helpers.
 */

#ifndef TEST_URMA_UVS_UVS_FIXTURE_H
#define TEST_URMA_UVS_UVS_FIXTURE_H

#include <cerrno>
#include <cstdint>
#include <cstdarg>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

using atomic_ulong = std::atomic_ulong;

extern "C" {
#include "tpsa_ioctl.h"
#include "tpsa_log.h"
#include "tpsa_topo_helper.h"
#include "uvs_api.h"
#include "uvs_cmd_tlv.h"
#include "uvs_private_api.h"
#include "uvs_ubagg_ioctl.h"
}

namespace urma_test_uvs {

struct MainUeMapping {
    uvs_eid_t eid;
    uvs_eid_t mainUeEid;
};

struct UvsIoctlCapture {
    bool succeed = true;
    bool mockDeviceOpen = false;
    bool statefulMainUe = false;
    int errnoValue = EINVAL;
    int32_t failCommand = -1;
    uint32_t command = 0;
    uint32_t argsLen = 0;
    uint32_t callCount = 0;
    uint32_t ubaggTopoNum = 1;
    std::vector<uint8_t> attrTypes;
    std::vector<uint16_t> fieldSizes;
    std::vector<std::vector<uint8_t>> attrData;
    std::vector<uvs_host_eid_batch_entry_t> hostBatches;
    std::vector<MainUeMapping> mainUeMappings;
    std::vector<urma_topo_node> topoNodes;
    urma_topo_node topoNode = {};
};

extern UvsIoctlCapture g_uvsIoctl;

inline void ResetUvSioctl()
{
    g_uvsIoctl = {};
    g_uvsIoctl.succeed = true;
    g_uvsIoctl.errnoValue = EINVAL;
}

inline uvs_eid_t MakeEid(uint8_t seed)
{
    uvs_eid_t eid = {};

    for (size_t i = 0; i < sizeof(eid.raw); i++) {
        eid.raw[i] = static_cast<uint8_t>(seed + i);
    }
    return eid;
}

inline bool EidsEqual(const uvs_eid_t &left, const uvs_eid_t &right)
{
    return std::memcmp(left.raw, right.raw, sizeof(left.raw)) == 0;
}

inline void FillTopoUe(struct urma_topo_ue *ue, uint32_t entityId, uint32_t chipId, uint32_t dieId,
    uint8_t primarySeed, uint8_t portSeed, uint8_t cnaSeed)
{
    uvs_eid_t primary = MakeEid(primarySeed);
    uvs_eid_t port = MakeEid(portSeed);
    uvs_eid_t cna = MakeEid(cnaSeed);

    ue->entity_id = entityId;
    ue->chip_id = chipId;
    ue->die_id = dieId;
    std::memcpy(ue->primary_eid, primary.raw, sizeof(primary.raw));
    std::memcpy(ue->port_eid[0], port.raw, sizeof(port.raw));
    std::memcpy(ue->cna[0], cna.raw, sizeof(cna.raw));
}

inline void FillTopoUePort(struct urma_topo_ue *ue, uint32_t portIdx, uint8_t portSeed, uint8_t cnaSeed)
{
    ASSERT_LT(portIdx, static_cast<uint32_t>(PORT_NUM));

    uvs_eid_t port = MakeEid(portSeed);
    uvs_eid_t cna = MakeEid(cnaSeed);

    std::memcpy(ue->port_eid[portIdx], port.raw, sizeof(port.raw));
    std::memcpy(ue->cna[portIdx], cna.raw, sizeof(cna.raw));
}

inline void ExpectEidEq(const uvs_eid_t &expected, const uvs_eid_t &actual)
{
    EXPECT_TRUE(EidsEqual(expected, actual));
}

inline void InitIoctlCtx(tpsa_ioctl_ctx_t *ctx)
{
    ctx->ubcore_fd = 7;
    ctx->id.store(0);
}

inline const uvs_cmd_attr_t *FindAttr(const uvs_cmd_attr_t *attrs, uint32_t attrCount, uint8_t type)
{
    for (uint32_t i = 0; i < attrCount; i++) {
        if (attrs[i].type == type) {
            return &attrs[i];
        }
    }
    return nullptr;
}

inline bool ReadAttrData(const uvs_cmd_attr_t *attrs, uint32_t attrCount, uint8_t type,
    void *out, size_t outSize)
{
    const uvs_cmd_attr_t *attr = FindAttr(attrs, attrCount, type);

    if (attr == nullptr || attr->data == 0 || attr->field_size < outSize) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<void *>(attr->data), outSize);
    return true;
}

inline bool WriteAttrData(const uvs_cmd_attr_t *attrs, uint32_t attrCount, uint8_t type,
    const void *in, size_t inSize)
{
    const uvs_cmd_attr_t *attr = FindAttr(attrs, attrCount, type);

    if (attr == nullptr || attr->data == 0 || attr->field_size < inSize) {
        return false;
    }
    std::memcpy(reinterpret_cast<void *>(attr->data), in, inSize);
    return true;
}

inline void StoreMainUeMapping(const uvs_eid_t &eid, const uvs_eid_t &mainUeEid)
{
    for (MainUeMapping &mapping : g_uvsIoctl.mainUeMappings) {
        if (EidsEqual(mapping.eid, eid)) {
            mapping.mainUeEid = mainUeEid;
            return;
        }
    }

    g_uvsIoctl.mainUeMappings.push_back({eid, mainUeEid});
}

inline bool LookupMainUeMapping(const uvs_eid_t &eid, uvs_eid_t *mainUeEid)
{
    for (const MainUeMapping &mapping : g_uvsIoctl.mainUeMappings) {
        if (EidsEqual(mapping.eid, eid)) {
            *mainUeEid = mapping.mainUeEid;
            return true;
        }
    }
    return false;
}

inline void DeleteMainUeMapping(const uvs_eid_t &eid)
{
    for (auto iter = g_uvsIoctl.mainUeMappings.begin();
         iter != g_uvsIoctl.mainUeMappings.end(); ++iter) {
        if (EidsEqual(iter->eid, eid)) {
            g_uvsIoctl.mainUeMappings.erase(iter);
            return;
        }
    }
}

inline int HandleStatefulMainUeCommand(const tpsa_cmd_hdr_t *hdr,
    const uvs_cmd_attr_t *attrs, uint32_t attrCount)
{
    uvs_eid_t eid = {};
    uvs_eid_t mainUeEid = {};
    uint32_t eidNum = 0;

    switch (hdr->command) {
        case UVS_CMD_INSERT_MAIN_UE_EID:
            if (!ReadAttrData(attrs, attrCount, INSERT_MAIN_UE_EID_IN_ENTRY_EID,
                &eid, sizeof(eid)) ||
                !ReadAttrData(attrs, attrCount, INSERT_MAIN_UE_EID_IN_ENTRY_MAIN_UE_EID,
                &mainUeEid, sizeof(mainUeEid))) {
                errno = EINVAL;
                return -1;
            }
            StoreMainUeMapping(eid, mainUeEid);
            return 0;
        case UVS_CMD_DELETE_MAIN_UE_EID:
            if (!ReadAttrData(attrs, attrCount, DELETE_MAIN_UE_EID_IN_EID,
                &eid, sizeof(eid))) {
                errno = EINVAL;
                return -1;
            }
            DeleteMainUeMapping(eid);
            return 0;
        case UVS_CMD_LOOKUP_MAIN_UE_EID:
            if (!ReadAttrData(attrs, attrCount, LOOKUP_MAIN_UE_EID_IN_EID,
                &eid, sizeof(eid))) {
                errno = EINVAL;
                return -1;
            }
            if (!LookupMainUeMapping(eid, &mainUeEid)) {
                errno = ENOENT;
                return -1;
            }
            if (!WriteAttrData(attrs, attrCount, LOOKUP_MAIN_UE_EID_OUT_MAIN_UE_EID,
                &mainUeEid, sizeof(mainUeEid))) {
                errno = EINVAL;
                return -1;
            }
            return 0;
        case UVS_CMD_FLUSH_MAIN_UE_EID: {
            int status = 1;

            g_uvsIoctl.mainUeMappings.clear();
            if (!WriteAttrData(attrs, attrCount, FLUSH_MAIN_UE_EID_OUT_STATUS,
                &status, sizeof(status))) {
                errno = EINVAL;
                return -1;
            }
            return 0;
        }
        case UVS_CMD_INSERT_MAIN_UE_EID_BATCH: {
            const uvs_cmd_attr_t *eidsAttr;
            const uvs_eid_t *eids;

            if (!ReadAttrData(attrs, attrCount, INSERT_MAIN_UE_EID_BATCH_IN_ENTRY_MAIN_UE_EID,
                &mainUeEid, sizeof(mainUeEid)) ||
                !ReadAttrData(attrs, attrCount, INSERT_MAIN_UE_EID_BATCH_IN_ENTRY_EID_NUM,
                &eidNum, sizeof(eidNum)) ||
                eidNum > UVS_MAIN_UE_EID_BATCH_EID_MAX) {
                errno = EINVAL;
                return -1;
            }
            eidsAttr = FindAttr(attrs, attrCount, INSERT_MAIN_UE_EID_BATCH_IN_ENTRY_EIDS);
            if (eidsAttr == nullptr || eidsAttr->data == 0 ||
                eidsAttr->field_size < eidNum * sizeof(uvs_eid_t)) {
                errno = EINVAL;
                return -1;
            }
            eids = reinterpret_cast<const uvs_eid_t *>(eidsAttr->data);
            for (uint32_t i = 0; i < eidNum; i++) {
                StoreMainUeMapping(eids[i], mainUeEid);
            }
            return 0;
        }
        default:
            return 0;
    }
}

inline void FillGenericOutAttrs(const uvs_cmd_attr_t *attrs, uint32_t attrCount)
{
    for (uint32_t i = 0; i < attrCount; i++) {
        if (attrs[i].type >= UVS_CMD_OUT_TYPE_INIT && attrs[i].data != 0 &&
            attrs[i].field_size <= sizeof(uint64_t)) {
            uint64_t value = 1;
            std::memcpy(reinterpret_cast<void *>(attrs[i].data), &value, attrs[i].field_size);
        }
    }
}

inline int CaptureTlvAttrs(const tpsa_cmd_hdr_t *hdr)
{
    auto *attrs = reinterpret_cast<uvs_cmd_attr_t *>(hdr->args_addr);
    uint32_t attrCount = hdr->args_len / sizeof(uvs_cmd_attr_t);

    g_uvsIoctl.command = hdr->command;
    g_uvsIoctl.argsLen = hdr->args_len;
    g_uvsIoctl.attrTypes.clear();
    g_uvsIoctl.fieldSizes.clear();
    g_uvsIoctl.attrData.clear();
    for (uint32_t i = 0; i < attrCount; i++) {
        g_uvsIoctl.attrTypes.push_back(attrs[i].type);
        g_uvsIoctl.fieldSizes.push_back(attrs[i].field_size);
        if (attrs[i].type == INSERT_HOST_EID_BATCH_IN_ENTRY &&
            attrs[i].field_size == sizeof(uvs_host_eid_batch_entry_t) &&
            attrs[i].data != 0) {
            uvs_host_eid_batch_entry_t entry = {};
            std::memcpy(&entry, reinterpret_cast<void *>(attrs[i].data), sizeof(entry));
            g_uvsIoctl.hostBatches.push_back(entry);
        }
    }

    if (g_uvsIoctl.statefulMainUe) {
        int ret = HandleStatefulMainUeCommand(hdr, attrs, attrCount);
        if (ret != 0) {
            return ret;
        }
    } else {
        FillGenericOutAttrs(attrs, attrCount);
    }

    for (uint32_t i = 0; i < attrCount; i++) {
        std::vector<uint8_t> data;
        if (attrs[i].data != 0 && attrs[i].field_size != 0) {
            auto *ptr = reinterpret_cast<uint8_t *>(attrs[i].data);
            data.assign(ptr, ptr + attrs[i].field_size);
        }
        g_uvsIoctl.attrData.push_back(data);
    }
    return 0;
}

inline int HandleUbaggIoctl(uvs_ubagg_cmd_hdr *hdr)
{
    if (hdr->command != UVS_UBAGG_CMD_GET_TOPO_INFO) {
        return 0;
    }

    auto *arg = reinterpret_cast<uvs_ubagg_get_topo_info_arg *>(hdr->args_addr);
    auto *topo = static_cast<urma_topo_node *>(arg->out.topo);
    uint32_t topoNum = g_uvsIoctl.topoNodes.empty() ?
        g_uvsIoctl.ubaggTopoNum : static_cast<uint32_t>(g_uvsIoctl.topoNodes.size());

    arg->out.topo_num = topoNum;
    if (!g_uvsIoctl.topoNodes.empty() && topoNum <= MAX_NODE_NUM) {
        for (uint32_t i = 0; i < topoNum; i++) {
            topo[i] = g_uvsIoctl.topoNodes[i];
        }
    } else if (topoNum <= MAX_NODE_NUM) {
        topo[0] = g_uvsIoctl.topoNode;
    }
    return 0;
}

inline void ExpectAttrTypes(std::initializer_list<uint8_t> expected)
{
    ASSERT_EQ(expected.size(), g_uvsIoctl.attrTypes.size());
    size_t index = 0;

    for (uint8_t type : expected) {
        EXPECT_EQ(type, g_uvsIoctl.attrTypes[index]);
        index++;
    }
}

class UrmaUvsTest : public testing::Test {
protected:
    void SetUp() override
    {
        ResetUvSioctl();
        uvs_set_worker_idx(-1);
        tpsa_log_set_level(TPSA_VLOG_LEVEL_INFO);
        unsetenv("UVS_LOG_LEVEL");
    }
};

} // namespace urma_test_uvs

#endif // TEST_URMA_UVS_UVS_FIXTURE_H
