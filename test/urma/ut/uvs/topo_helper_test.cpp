/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UVS topology helper unit tests.
 */

#include "uvs_fixture.h"

using namespace urma_test_uvs;

TEST_F(UrmaUvsTest, TopoHelpersHandleStableBoundaryInput)
{
    auto *node = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, node);

    EXPECT_EQ(0, uvs_update_main_ue_eid_table_by_topo(nullptr, 0));
    FillTopoUe(&node->agg_devs[0].ues[0], 3, 1, 0, 11, 12, 13);
    EXPECT_EQ(-1, uvs_update_main_ue_eid_table_by_topo(node, 1));
    EXPECT_EQ(-EINVAL, uvs_update_host_eid_table_by_share_topo(nullptr, 1));
    EXPECT_EQ(-EINVAL, uvs_update_host_eid_table_by_share_topo(node, 0));
    EXPECT_EQ(-EINVAL, uvs_update_host_eid_table_by_share_topo(node, MAX_NODE_NUM + 1));

    std::free(node);
}

TEST_F(UrmaUvsTest, TopoHelpersUpdateMainUeBatchesWithMockedUbcore)
{
    auto *node = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, node);

    g_uvsIoctl.mockDeviceOpen = true;
    FillTopoUe(&node->agg_devs[0].ues[0], 20, 2, 0, 31, 32, 33);
    FillTopoUe(&node->agg_devs[0].ues[1], 18, 1, 0, 29, 34, 35);
    FillTopoUe(&node->agg_devs[1].ues[0], 23, 4, 0, 36, 37, 38);
    FillTopoUe(&node->agg_devs[1].ues[1], 20, 2, 0, 28, 39, 40);

    EXPECT_EQ(0, uvs_update_main_ue_eid_table_by_topo(node, 1));
    EXPECT_GT(g_uvsIoctl.callCount, 0U);

    std::free(node);
}

TEST_F(UrmaUvsTest, TopoHelpersBuildLookupableMainUeMappingsFromTopo)
{
    auto *node = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, node);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.statefulMainUe = true;
    FillTopoUe(&node->agg_devs[0].ues[0], 20, 2, 0, 61, 62, 63);
    FillTopoUePort(&node->agg_devs[0].ues[0], 1, 64, 65);
    FillTopoUe(&node->agg_devs[1].ues[0], 20, 2, 1, 41, 42, 43);
    FillTopoUe(&node->agg_devs[2].ues[0], 21, 3, 0, 71, 72, 73);

    EXPECT_EQ(0, uvs_update_main_ue_eid_table_by_topo(node, 1));

    const uvs_eid_t expectedSharedMain = MakeEid(41);
    const uvs_eid_t expectedIndependentMain = MakeEid(71);
    const uvs_eid_t sharedEids[] = {
        MakeEid(61), MakeEid(62), MakeEid(64), MakeEid(41), MakeEid(42),
    };
    for (const uvs_eid_t &eid : sharedEids) {
        uvs_eid_t outEid = {};
        EXPECT_EQ(0, uvs_lookup_main_ue_eid(&eid, &outEid));
        ExpectEidEq(expectedSharedMain, outEid);
    }

    const uvs_eid_t independentEids[] = {MakeEid(71), MakeEid(72)};
    for (const uvs_eid_t &eid : independentEids) {
        uvs_eid_t outEid = {};
        EXPECT_EQ(0, uvs_lookup_main_ue_eid(&eid, &outEid));
        ExpectEidEq(expectedIndependentMain, outEid);
    }

    std::free(node);
}

TEST_F(UrmaUvsTest, TopoHelpersPropagateMainUeBatchIoctlFailure)
{
    auto *node = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, node);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.failCommand = UVS_CMD_INSERT_MAIN_UE_EID_BATCH;
    g_uvsIoctl.errnoValue = ENODEV;
    tpsa_log_set_level(TPSA_VLOG_LEVEL_CRIT);
    FillTopoUe(&node->agg_devs[0].ues[0], 4, 1, 0, 51, 52, 53);

    EXPECT_EQ(-ENODEV, uvs_update_main_ue_eid_table_by_topo(node, 1));

    std::free(node);
}

TEST_F(UrmaUvsTest, TopoHelpersBuildHostEidMappingsWithMockedDevices)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x31;
    g_uvsIoctl.topoNode.is_current = 1;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 7, 2, 1, 20, 22, 23);
    shareNode->node_id = 0x31;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 7, 2, 1, 21, 21, 24);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    EXPECT_GE(g_uvsIoctl.callCount, 2U);

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersAggregateSameHostEidAcrossNormalDevices)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x41;
    g_uvsIoctl.topoNode.is_current = 1;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 17, 3, 1, 71, 81, 91);
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[1].ues[0], 17, 3, 1, 72, 82, 92);
    shareNode->node_id = 0x41;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 17, 3, 1, 73, 83, 93);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    ASSERT_EQ(1U, g_uvsIoctl.hostBatches.size());
    const uvs_host_eid_batch_entry_t &batch = g_uvsIoctl.hostBatches[0];
    EXPECT_EQ(2U, batch.eid_num);
    ExpectEidEq(MakeEid(83), batch.host_eid);
    ExpectEidEq(MakeEid(81), batch.eids[0]);
    ExpectEidEq(MakeEid(82), batch.eids[1]);
    ExpectEidEq(MakeEid(91), batch.cnas[0]);
    ExpectEidEq(MakeEid(92), batch.cnas[1]);

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersBuildHostEidMappingsAcrossMultipleNodes)
{
    auto *shareNodes = static_cast<urma_topo_node *>(std::calloc(2, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNodes);

    g_uvsIoctl.mockDeviceOpen = true;
    urma_topo_node topoNodeA = {};
    topoNodeA.node_id = 0x44;
    FillTopoUe(&topoNodeA.agg_devs[0].ues[0], 31, 1, 1, 101, 111, 121);
    urma_topo_node topoNodeB = {};
    topoNodeB.node_id = 0x45;
    FillTopoUe(&topoNodeB.agg_devs[0].ues[0], 32, 1, 1, 102, 112, 122);
    g_uvsIoctl.topoNodes.push_back(topoNodeA);
    g_uvsIoctl.topoNodes.push_back(topoNodeB);

    shareNodes[0].node_id = 0x44;
    FillTopoUe(&shareNodes[0].agg_devs[0].ues[0], 31, 1, 1, 103, 113, 123);
    shareNodes[1].node_id = 0x45;
    FillTopoUe(&shareNodes[1].agg_devs[0].ues[0], 32, 1, 1, 104, 114, 124);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNodes, 2));
    ASSERT_EQ(2U, g_uvsIoctl.hostBatches.size());
    EXPECT_EQ(1U, g_uvsIoctl.hostBatches[0].eid_num);
    ExpectEidEq(MakeEid(113), g_uvsIoctl.hostBatches[0].host_eid);
    ExpectEidEq(MakeEid(111), g_uvsIoctl.hostBatches[0].eids[0]);
    ExpectEidEq(MakeEid(121), g_uvsIoctl.hostBatches[0].cnas[0]);
    EXPECT_EQ(1U, g_uvsIoctl.hostBatches[1].eid_num);
    ExpectEidEq(MakeEid(114), g_uvsIoctl.hostBatches[1].host_eid);
    ExpectEidEq(MakeEid(112), g_uvsIoctl.hostBatches[1].eids[0]);
    ExpectEidEq(MakeEid(122), g_uvsIoctl.hostBatches[1].cnas[0]);

    std::free(shareNodes);
}

TEST_F(UrmaUvsTest, TopoHelpersBuildHostEidMappingsAcrossPorts)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x46;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 33, 2, 1, 105, 115, 125);
    FillTopoUePort(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 1, 116, 126);
    shareNode->node_id = 0x46;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 33, 2, 1, 106, 117, 127);
    FillTopoUePort(&shareNode->agg_devs[0].ues[0], 1, 118, 128);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    ASSERT_EQ(2U, g_uvsIoctl.hostBatches.size());
    ExpectEidEq(MakeEid(117), g_uvsIoctl.hostBatches[0].host_eid);
    ExpectEidEq(MakeEid(115), g_uvsIoctl.hostBatches[0].eids[0]);
    ExpectEidEq(MakeEid(125), g_uvsIoctl.hostBatches[0].cnas[0]);
    ExpectEidEq(MakeEid(118), g_uvsIoctl.hostBatches[1].host_eid);
    ExpectEidEq(MakeEid(116), g_uvsIoctl.hostBatches[1].eids[0]);
    ExpectEidEq(MakeEid(126), g_uvsIoctl.hostBatches[1].cnas[0]);

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersSplitFullHostEidBatchForSameHost)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x42;
    g_uvsIoctl.topoNode.is_current = 1;
    for (uint32_t devIdx = 0; devIdx < UVS_HOST_EID_BATCH_EID_MAX + 1; devIdx++) {
        FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[devIdx].ues[0], 19, 4, 1,
            static_cast<uint8_t>(10 + devIdx), static_cast<uint8_t>(50 + devIdx),
            static_cast<uint8_t>(100 + devIdx));
    }
    shareNode->node_id = 0x42;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 19, 4, 1, 180, 190, 200);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    ASSERT_EQ(2U, g_uvsIoctl.hostBatches.size());
    EXPECT_EQ(UVS_HOST_EID_BATCH_EID_MAX, g_uvsIoctl.hostBatches[0].eid_num);
    EXPECT_EQ(1U, g_uvsIoctl.hostBatches[1].eid_num);
    ExpectEidEq(MakeEid(190), g_uvsIoctl.hostBatches[0].host_eid);
    ExpectEidEq(MakeEid(190), g_uvsIoctl.hostBatches[1].host_eid);
    ExpectEidEq(MakeEid(50), g_uvsIoctl.hostBatches[0].eids[0]);
    ExpectEidEq(MakeEid(50 + UVS_HOST_EID_BATCH_EID_MAX),
        g_uvsIoctl.hostBatches[1].eids[0]);
    ExpectEidEq(MakeEid(100 + UVS_HOST_EID_BATCH_EID_MAX),
        g_uvsIoctl.hostBatches[1].cnas[0]);

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersRejectWhenHostEidBatchTableIsFull)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x43;
    constexpr uint32_t batchOverflowCount = 129;
    for (uint32_t devIdx = 0; devIdx < batchOverflowCount; devIdx++) {
        FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[devIdx].ues[0], 1000 + devIdx, 5, 1,
            static_cast<uint8_t>(1 + devIdx), static_cast<uint8_t>(130 + devIdx),
            static_cast<uint8_t>(20 + devIdx));
        FillTopoUe(&shareNode->agg_devs[devIdx].ues[0], 1000 + devIdx, 5, 1,
            static_cast<uint8_t>(30 + devIdx), static_cast<uint8_t>(1 + devIdx),
            static_cast<uint8_t>(60 + devIdx));
    }
    shareNode->node_id = 0x43;

    EXPECT_EQ(-ENOSPC, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    EXPECT_EQ(1U, g_uvsIoctl.callCount);
    EXPECT_TRUE(g_uvsIoctl.hostBatches.empty());

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersIgnoreEmptySharePortsAndRejectEmptyNormalPort)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x47;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 34, 2, 1, 131, 132, 133);
    shareNode->node_id = 0x47;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 34, 2, 1, 134, 135, 136);
    std::memset(shareNode->agg_devs[0].ues[0].port_eid[0], 0, EID_LEN);

    EXPECT_EQ(0, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    EXPECT_TRUE(g_uvsIoctl.hostBatches.empty());

    ResetUvSioctl();
    std::memset(shareNode, 0, sizeof(*shareNode));
    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x48;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 35, 2, 1, 137, 138, 139);
    std::memset(g_uvsIoctl.topoNode.agg_devs[0].ues[0].port_eid[0], 0, EID_LEN);
    shareNode->node_id = 0x48;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 35, 2, 1, 140, 141, 142);

    EXPECT_EQ(-ENOENT, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    EXPECT_TRUE(g_uvsIoctl.hostBatches.empty());

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersPropagateHostEidBatchIoctlFailure)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.failCommand = UVS_CMD_INSERT_HOST_EID_BATCH;
    g_uvsIoctl.errnoValue = ENODEV;
    tpsa_log_set_level(TPSA_VLOG_LEVEL_CRIT);
    g_uvsIoctl.topoNode.node_id = 0x49;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 36, 3, 1, 143, 144, 145);
    shareNode->node_id = 0x49;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 36, 3, 1, 146, 147, 148);

    EXPECT_EQ(-ENODEV, uvs_update_host_eid_table_by_share_topo(shareNode, 1));
    EXPECT_TRUE(g_uvsIoctl.hostBatches.empty());

    std::free(shareNode);
}

TEST_F(UrmaUvsTest, TopoHelpersRejectInvalidOrUnmatchedShareTopo)
{
    auto *shareNode = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, shareNode);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.ubaggTopoNum = 0;
    EXPECT_EQ(-EINVAL, uvs_update_host_eid_table_by_share_topo(shareNode, 1));

    ResetUvSioctl();
    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x90;
    shareNode->node_id = 0x91;
    EXPECT_EQ(-ENOENT, uvs_update_host_eid_table_by_share_topo(shareNode, 1));

    ResetUvSioctl();
    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.topoNode.node_id = 0x92;
    FillTopoUe(&g_uvsIoctl.topoNode.agg_devs[0].ues[0], 1, 1, 1, 61, 62, 63);
    shareNode->node_id = 0x92;
    FillTopoUe(&shareNode->agg_devs[0].ues[0], 2, 1, 1, 64, 65, 66);
    EXPECT_EQ(-ENOENT, uvs_update_host_eid_table_by_share_topo(shareNode, 1));

    std::free(shareNode);
}
