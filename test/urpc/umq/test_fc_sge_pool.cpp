/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: FC SGE pool and SGE flow control unit tests
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include <cstring>
#include <malloc.h>

#include "umq_ub_fc_sge_pool.h"
#include "umq_ub_flow_control.h"
#include "umq_ub_imm_data.h"
#include "umq_ub_private.h"

#define TEST_FC_SGE_POOL_SIZE 64
#define TEST_QUEUE_RX_DEPTH 128

static umq_buf_t *g_mock_qbuf_list[TEST_FC_SGE_POOL_SIZE] = {0};
static int g_mock_qbuf_count = 0;

static umq_buf_t *mock_umq_buf_alloc(uint32_t size, uint32_t num, uint64_t umqh, void *option)
{
    umq_buf_t *qbuf = (umq_buf_t *)calloc(1, sizeof(umq_buf_t) + size);
    if (qbuf == nullptr) {
        return nullptr;
    }
    qbuf->buf_data = (char *)(qbuf + 1);
    qbuf->buf_size = size;
    if (g_mock_qbuf_count < TEST_FC_SGE_POOL_SIZE) {
        g_mock_qbuf_list[g_mock_qbuf_count++] = qbuf;
    }
    return qbuf;
}

static void mock_umq_buf_free(umq_buf_t *qbuf)
{
    if (qbuf != nullptr) {
        free(qbuf);
    }
}

class FcSgePoolTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        g_mock_qbuf_count = 0;
        memset(g_mock_qbuf_list, 0, sizeof(g_mock_qbuf_list));
        MOCKER(umq_buf_alloc).stubs().will(invoke(mock_umq_buf_alloc));
        MOCKER(umq_buf_free).stubs().will(invoke(mock_umq_buf_free));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(FcSgePoolTest, test_pool_create_destroy)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);
    ASSERT_NE(pool->bitmap, nullptr);
    ASSERT_NE(pool->qbuf_array, nullptr);
    ASSERT_NE(pool->lock, nullptr);
    ASSERT_EQ(pool->qbuf_count, 0);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_pool_create_null_alloc)
{
    MOCKER(umq_buf_alloc).stubs().will(returnValue((umq_buf_t *)nullptr));
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_alloc_free_single)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_ub_fc_sge_slot_t slot = {0};
    int ret = umq_ub_fc_sge_alloc(pool, &slot);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(slot.addr, nullptr);
    ASSERT_EQ(slot.len, UMQ_UB_FC_SGE_SLOT_SIZE);
    ASSERT_LT(slot.bitmap_pos, UMQ_UB_FC_SGE_POOL_MAX_SLOTS);

    umq_ub_fc_sge_free(pool, &slot);
    ASSERT_EQ(slot.addr, nullptr);
    ASSERT_EQ(slot.len, 0);

    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_alloc_free_multiple)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    const int num_slots = 10;
    umq_ub_fc_sge_slot_t slots[num_slots] = {};
    for (int i = 0; i < num_slots; i++) {
        int ret = umq_ub_fc_sge_alloc(pool, &slots[i]);
        ASSERT_EQ(ret, UMQ_SUCCESS);
        ASSERT_NE(slots[i].addr, nullptr);
    }

    for (int i = 0; i < num_slots; i++) {
        umq_ub_fc_sge_free(pool, &slots[i]);
        ASSERT_EQ(slots[i].addr, nullptr);
    }

    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_alloc_null_pool)
{
    umq_ub_fc_sge_slot_t slot = {0};
    int ret = umq_ub_fc_sge_alloc(NULL, &slot);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);
}

TEST_F(FcSgePoolTest, test_sge_free_null_slot)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);
    umq_ub_fc_sge_free(pool, NULL);
    umq_ub_fc_sge_free(NULL, NULL);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_ctx_init_non_shared)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_ub_fc_sge_ctx_t ctx = {};
    bool need_rx = true;
    int ret = umq_ub_fc_sge_ctx_init(pool, &ctx, need_rx);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(ctx.tx_slot.addr, nullptr);
    ASSERT_EQ(ctx.rx_slot_count, 2);
    for (uint32_t i = 0; i < ctx.rx_slot_count; i++) {
        ASSERT_NE(ctx.rx_slots[i].addr, nullptr);
    }

    umq_ub_fc_sge_ctx_uninit(pool, &ctx);
    ASSERT_EQ(ctx.tx_slot.addr, nullptr);
    ASSERT_EQ(ctx.rx_slot_count, 0);

    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_ctx_init_shared_no_rx)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_ub_fc_sge_ctx_t ctx = {};
    bool need_rx = false;
    int ret = umq_ub_fc_sge_ctx_init(pool, &ctx, need_rx);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(ctx.tx_slot.addr, nullptr);
    ASSERT_EQ(ctx.rx_slot_count, 0);

    umq_ub_fc_sge_ctx_uninit(pool, &ctx);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_ctx_init_null_params)
{
    umq_ub_fc_sge_ctx_t ctx = {};
    int ret = umq_ub_fc_sge_ctx_init(NULL, &ctx, true);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);

    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);
    ret = umq_ub_fc_sge_ctx_init(pool, NULL, true);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_reuse_after_free)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_ub_fc_sge_slot_t slot1 = {0};
    int ret = umq_ub_fc_sge_alloc(pool, &slot1);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    uint32_t first_pos = slot1.bitmap_pos;

    umq_ub_fc_sge_free(pool, &slot1);

    umq_ub_fc_sge_slot_t slot2 = {0};
    ret = umq_ub_fc_sge_alloc(pool, &slot2);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_EQ(slot2.bitmap_pos, first_pos);

    umq_ub_fc_sge_free(pool, &slot2);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgePoolTest, test_sge_write_read_data)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_ub_fc_sge_slot_t slot = {0};
    int ret = umq_ub_fc_sge_alloc(pool, &slot);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    umq_ub_fc_sge_msg_t write_msg = {
        .window = 1024,
        .ratio = 2,
        .rsvd = 0,
    };
    *(umq_ub_fc_sge_msg_t *)slot.addr = write_msg;

    umq_ub_fc_sge_msg_t read_msg = *(umq_ub_fc_sge_msg_t *)slot.addr;
    ASSERT_EQ(read_msg.window, 1024u);
    ASSERT_EQ(read_msg.ratio, 2u);

    umq_ub_fc_sge_free(pool, &slot);
    umq_ub_fc_sge_pool_destroy(pool);
}

class FcSgeSharedJfrTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        g_mock_qbuf_count = 0;
        memset(g_mock_qbuf_list, 0, sizeof(g_mock_qbuf_list));
        MOCKER(umq_buf_alloc).stubs().will(invoke(mock_umq_buf_alloc));
        MOCKER(umq_buf_free).stubs().will(invoke(mock_umq_buf_free));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(FcSgeSharedJfrTest, test_shared_jfr_rx_sge_init_uninit)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_buf_t **qbufs = nullptr;
    uint32_t qbuf_count = 0;
    void **sge_addrs = nullptr;
    uint32_t sge_count = 0;
    uint32_t depth = 32;

    int ret = umq_ub_fc_shared_jfr_rx_sge_init(pool, depth, &qbufs, &qbuf_count, &sge_addrs, &sge_count);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(qbufs, nullptr);
    ASSERT_GT(qbuf_count, 0u);
    ASSERT_NE(sge_addrs, nullptr);
    ASSERT_EQ(sge_count, depth);

    for (uint32_t i = 0; i < sge_count; i++) {
        ASSERT_NE(sge_addrs[i], nullptr);
    }

    umq_ub_fc_shared_jfr_rx_sge_uninit(qbufs, qbuf_count, sge_addrs, sge_count);
    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgeSharedJfrTest, test_shared_jfr_rx_sge_init_invalid_depth)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_buf_t **qbufs = nullptr;
    uint32_t qbuf_count = 0;
    void **sge_addrs = nullptr;
    uint32_t sge_count = 0;

    int ret = umq_ub_fc_shared_jfr_rx_sge_init(NULL, 32, &qbufs, &qbuf_count, &sge_addrs, &sge_count);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);

    ret = umq_ub_fc_shared_jfr_rx_sge_init(pool, 0, &qbufs, &qbuf_count, &sge_addrs, &sge_count);
    ASSERT_EQ(ret, -UMQ_ERR_EINVAL);

    umq_ub_fc_sge_pool_destroy(pool);
}

TEST_F(FcSgeSharedJfrTest, test_shared_jfr_rx_sge_write_read)
{
    umq_ub_fc_sge_pool_t *pool = umq_ub_fc_sge_pool_create();
    ASSERT_NE(pool, nullptr);

    umq_buf_t **qbufs = nullptr;
    uint32_t qbuf_count = 0;
    void **sge_addrs = nullptr;
    uint32_t sge_count = 0;
    uint32_t depth = 32;

    int ret = umq_ub_fc_shared_jfr_rx_sge_init(pool, depth, &qbufs, &qbuf_count, &sge_addrs, &sge_count);
    ASSERT_EQ(ret, UMQ_SUCCESS);

    for (uint32_t i = 0; i < sge_count; i++) {
        umq_ub_fc_sge_msg_t msg = {
            .window = i * 10,
            .ratio = i % 4,
            .rsvd = 0,
        };
        *(umq_ub_fc_sge_msg_t *)sge_addrs[i] = msg;
    }

    for (uint32_t i = 0; i < sge_count; i++) {
        umq_ub_fc_sge_msg_t read_msg = *(umq_ub_fc_sge_msg_t *)sge_addrs[i];
        ASSERT_EQ(read_msg.window, i * 10u);
        ASSERT_EQ(read_msg.ratio, i % 4u);
    }

    umq_ub_fc_shared_jfr_rx_sge_uninit(qbufs, qbuf_count, sge_addrs, sge_count);
    umq_ub_fc_sge_pool_destroy(pool);
}

class FcImmFormatTest : public ::testing::Test {
  public:
    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(FcImmFormatTest, test_fc_imm_type_credit_req)
{
    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REQ;
    fc_imm.bs.umq_id = 0x3FFFF;
    fc_imm.bs.rsvd = 0;

    ASSERT_EQ(fc_imm.bs.type, FC_IMM_TYPE_CREDIT_REQ);
    ASSERT_EQ(fc_imm.bs.umq_id, 0x3FFFFu);

    uint32_t raw = fc_imm.value;
    umq_ub_fc_imm_t parsed = {.value = raw};
    ASSERT_EQ(parsed.bs.type, FC_IMM_TYPE_CREDIT_REQ);
    ASSERT_EQ(parsed.bs.umq_id, 0x3FFFFu);
}

TEST_F(FcImmFormatTest, test_fc_imm_type_credit_rep)
{
    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REP;
    fc_imm.bs.umq_id = 0x100;
    fc_imm.bs.rsvd = 0;

    ASSERT_EQ(fc_imm.bs.type, FC_IMM_TYPE_CREDIT_REP);
    ASSERT_EQ(fc_imm.bs.umq_id, 0x100u);
}

TEST_F(FcImmFormatTest, test_fc_imm_type_return_req)
{
    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_RETURN_REQ;
    fc_imm.bs.umq_id = 0x2ABCD;
    fc_imm.bs.rsvd = 0;

    ASSERT_EQ(fc_imm.bs.type, FC_IMM_TYPE_CREDIT_RETURN_REQ);
    ASSERT_EQ(fc_imm.bs.umq_id, 0x2ABCDu);
}

TEST_F(FcImmFormatTest, test_fc_imm_type_return_ack)
{
    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_RETURN_ACK;
    fc_imm.bs.umq_id = 0x1;
    fc_imm.bs.rsvd = 0;

    ASSERT_EQ(fc_imm.bs.type, FC_IMM_TYPE_CREDIT_RETURN_ACK);
    ASSERT_EQ(fc_imm.bs.umq_id, 0x1u);
}

TEST_F(FcImmFormatTest, test_fc_sge_msg_format)
{
    umq_ub_fc_sge_msg_t sge_msg = {};
    sge_msg.window = 0xFFFFF;
    sge_msg.ratio = 0xF;
    sge_msg.rsvd = 0;

    ASSERT_EQ(sge_msg.window, 0xFFFFFu);
    ASSERT_EQ(sge_msg.ratio, 0xFu);
}

TEST_F(FcImmFormatTest, test_fc_imm_max_umq_id)
{
    ASSERT_EQ(UMQ_UB_FC_MAX_UMQ_ID, (1 << UMQ_UB_FC_UMQ_ID_BITS) - 1);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.umq_id = UMQ_UB_FC_MAX_UMQ_ID;
    ASSERT_EQ(fc_imm.bs.umq_id, UMQ_UB_FC_MAX_UMQ_ID);
}

TEST_F(FcImmFormatTest, test_fc_sge_msg_window_range)
{
    umq_ub_fc_sge_msg_t sge_msg = {};
    sge_msg.window = UMQ_UB_FC_MAX_WINDOW;
    ASSERT_EQ(sge_msg.window, UMQ_UB_FC_MAX_WINDOW);
    ASSERT_EQ(sge_msg.window, 0xFFFFFu);
}

class FcSgeFlowControlTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        g_mock_qbuf_count = 0;
        memset(g_mock_qbuf_list, 0, sizeof(g_mock_qbuf_list));
        MOCKER(umq_buf_alloc).stubs().will(invoke(mock_umq_buf_alloc));
        MOCKER(umq_buf_free).stubs().will(invoke(mock_umq_buf_free));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(FcSgeFlowControlTest, test_fc_sge_credit_req_handle)
{
    ub_queue_t queue = {};
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    umq_ub_shared_credit_recharge(&queue, 16);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REQ;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {};
    sge_msg.window = 4;

    umq_ub_fc_sge_credit_req_handle(&queue, &fc_imm, &sge_msg);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(FcSgeFlowControlTest, test_fc_sge_credit_resp_handle)
{
    ub_queue_t queue = {};
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    queue.flow_control.credits_per_request = 4;
    queue.flow_control.credit_multiple = 2.0;
    queue.flow_control.max_credits_request = 512;

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REP;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {};
    sge_msg.window = 4;
    sge_msg.ratio = 0;

    uint16_t old_window = queue.flow_control.remote_rx_window;
    umq_ub_fc_sge_credit_resp_handle(&queue, &fc_imm, &sge_msg);
    ASSERT_EQ(queue.flow_control.remote_rx_window, old_window + 4);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(FcSgeFlowControlTest, test_fc_sge_credit_return_req_handle)
{
    ub_queue_t queue = {};
    umq_ub_ctx_t dev_ctx = {};
    queue.dev_ctx = &dev_ctx;
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    urma_jetty_t jetty = {};
    urma_jetty_id_t jetty_id = {.id = 0};
    jetty.jetty_id = jetty_id;
    queue.jetty[UB_QUEUE_JETTY_IO] = &jetty;
    queue.jetty[UB_QUEUE_JETTY_FLOW_CONTROL] = &jetty;

    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    umq_ub_shared_credit_recharge(&queue, 16);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_RETURN_REQ;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {};
    sge_msg.window = 4;

    umq_ub_fc_sge_credit_return_req_handle(&queue, &fc_imm, &sge_msg);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(FcSgeFlowControlTest, test_fc_sge_pool_destroy_null)
{
    umq_ub_fc_sge_pool_destroy(NULL);
}

TEST_F(FcSgeFlowControlTest, test_fc_sge_ctx_uninit_null)
{
    umq_ub_fc_sge_ctx_uninit(NULL, NULL);
}

class FcUmqIdRoutingTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        memset(&dev_ctx, 0, sizeof(dev_ctx));
        dev_ctx.umq_id_to_queue_size = 256;
        dev_ctx.umq_id_to_queue_table = (volatile uint64_t *)calloc(
            dev_ctx.umq_id_to_queue_size, sizeof(uint64_t));
        ASSERT_NE(dev_ctx.umq_id_to_queue_table, nullptr);

        memset(&queue1, 0, sizeof(queue1));
        queue1.dev_ctx = &dev_ctx;
        queue1.umq_id = 1;

        memset(&queue2, 0, sizeof(queue2));
        queue2.dev_ctx = &dev_ctx;
        queue2.umq_id = 2;
    }

    void TearDown() override
    {
        if (dev_ctx.umq_id_to_queue_table != nullptr) {
            free((void *)dev_ctx.umq_id_to_queue_table);
        }
        GlobalMockObject::verify();
    }

    umq_ub_ctx_t dev_ctx;
    ub_queue_t queue1;
    ub_queue_t queue2;
};

TEST_F(FcUmqIdRoutingTest, test_register_and_lookup)
{
    umq_ub_register_queue_by_umq_id(&queue1);
    umq_ub_register_queue_by_umq_id(&queue2);

    ub_queue_t *found = umq_ub_get_queue_by_umq_id(&queue1, 1);
    ASSERT_EQ(found, &queue1);

    found = umq_ub_get_queue_by_umq_id(&queue1, 2);
    ASSERT_EQ(found, &queue2);

    found = umq_ub_get_queue_by_umq_id(&queue1, 3);
    ASSERT_EQ(found, nullptr);
}

TEST_F(FcUmqIdRoutingTest, test_unregister)
{
    umq_ub_register_queue_by_umq_id(&queue1);
    umq_ub_unregister_queue_by_umq_id(&queue1);

    ub_queue_t *found = umq_ub_get_queue_by_umq_id(&queue1, 1);
    ASSERT_EQ(found, nullptr);
}

TEST_F(FcUmqIdRoutingTest, test_register_overwrite)
{
    umq_ub_register_queue_by_umq_id(&queue1);
    umq_ub_register_queue_by_umq_id(&queue2);

    ub_queue_t *found = umq_ub_get_queue_by_umq_id(&queue1, 2);
    ASSERT_EQ(found, &queue2);
}

TEST_F(FcUmqIdRoutingTest, test_lookup_invalid_id)
{
    umq_ub_register_queue_by_umq_id(&queue1);

    ub_queue_t *found = umq_ub_get_queue_by_umq_id(&queue1, dev_ctx.umq_id_to_queue_size);
    ASSERT_EQ(found, nullptr);
}

class FcProcessSgeMsgTest : public ::testing::Test {
  public:
    void SetUp() override
    {
        g_mock_qbuf_count = 0;
        memset(g_mock_qbuf_list, 0, sizeof(g_mock_qbuf_list));
        MOCKER(umq_buf_alloc).stubs().will(invoke(mock_umq_buf_alloc));
        MOCKER(umq_buf_free).stubs().will(invoke(mock_umq_buf_free));
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(FcProcessSgeMsgTest, test_process_credit_req)
{
    ub_queue_t queue = {};
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    umq_ub_shared_credit_recharge(&queue, 16);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REQ;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {.window = 4, .ratio = 0, .rsvd = 0};
    umq_buf_t *buf = nullptr;
    int result = umq_ub_process_fc_sge_msg(&queue, fc_imm, &sge_msg, &buf);
    ASSERT_EQ(result, 0);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(FcProcessSgeMsgTest, test_process_credit_rep)
{
    ub_queue_t queue = {};
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    queue.flow_control.credits_per_request = 4;
    queue.flow_control.credit_multiple = 2.0;
    queue.flow_control.max_credits_request = 512;
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_CREDIT_REP;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {.window = 4, .ratio = 0, .rsvd = 0};
    umq_buf_t *buf = nullptr;
    int result = umq_ub_process_fc_sge_msg(&queue, fc_imm, &sge_msg, &buf);
    ASSERT_EQ(result, 0);

    umq_ub_flow_control_uninit(&queue.flow_control);
}

TEST_F(FcProcessSgeMsgTest, test_process_unknown_type)
{
    ub_queue_t queue = {};
    queue.flow_control.local_rx_depth = TEST_QUEUE_RX_DEPTH;
    jfr_ctx_t io_jfr_ctx = {};
    queue.jfr_ctx[UB_QUEUE_JETTY_IO] = &io_jfr_ctx;
    umq_flow_control_cfg_t cfg = {.use_atomic_window = true};
    int ret = umq_ub_flow_control_init(&queue.flow_control, &queue, UMQ_FEATURE_ENABLE_FLOW_CONTROL, &cfg);
    ASSERT_EQ(ret, 0);

    umq_ub_fc_imm_t fc_imm = {};
    fc_imm.bs.type = FC_IMM_TYPE_MAX;
    fc_imm.bs.umq_id = 1;

    umq_ub_fc_sge_msg_t sge_msg = {};
    umq_buf_t *buf = nullptr;
    int result = umq_ub_process_fc_sge_msg(&queue, fc_imm, &sge_msg, &buf);
    ASSERT_EQ(result, 0);

    umq_ub_flow_control_uninit(&queue.flow_control);
}
