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

TEST_F(UrmaUvsTest, TopoHelpersPropagateMainUeBatchIoctlFailure)
{
    auto *node = static_cast<urma_topo_node *>(std::calloc(1, sizeof(urma_topo_node)));
    ASSERT_NE(nullptr, node);

    g_uvsIoctl.mockDeviceOpen = true;
    g_uvsIoctl.succeed = false;
    g_uvsIoctl.errnoValue = ENODEV;
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
