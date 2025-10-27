/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc queue test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <deque>
#include <future>
#include <iostream>
#include <random>

#include "notify.h"
#include "async_event.h"
#include "cp.h"
#include "dp.h"
#include "jetty_public_func.h"

#include "protocol.h"
#include "provider_ops_jetty.h"
#include "queue.h"
#include "queue_send_recv.h"
#include "state.h"
#include "urma_api.h"
#include "urpc_framework_api.h"
#include "urpc_dbuf_stat.h"
#include "urpc_framework_errno.h"

#include "urpc_manage.h"
#include "urpc_timer.h"

#define DEFAULT_PRIORITY 5
#define DEFAULT_RX_DEPTH 128
#define DEFAULT_TX_DEPTH 128
#define DEFAULT_MSG_SIZE 64

#define SHARED_JFR_Q1_MAX_RX_SGE 16
#define SHARED_JFR_Q1_RX_DEPTH 32
#define SHARED_JFR_Q1_RX_BUF_SIZE 128

#define SHARED_JFR_Q2_MAX_RX_SGE 15
#define SHARED_JFR_Q2_RX_DEPTH 31
#define SHARED_JFR_Q2_RX_BUF_SIZE 127

#define SHARED_JFC_MAX_RX_SGE 15
#define SHARED_JFC_RX_BUF_SIZE 127

#define MAX_MSG_SIZE (1UL << 20)
#define READ_CACHE_LIST_TEST_CONCURRENT_CNT 4
#define DEFAULT_POLL_NUM 32
#define SGE_SIZE 4096
#define SGE_CUR 3
#define PLOG_SGE_SIZE_ARRAY_SIZE 32
#define URPC_EXT_HEADER_SIZE 256
#define SIMULATE_PLOG_CMD_SIZE 128
#define FAKE_MEM_HANDLE 0x123
#define PLOG_READ_HEADER_ROOM_SIZE 64
#define PLOG_READ_HEADER_TOTAL_SIZE 256
#define URPC_TIMER_MAGIC_NUM 0x33445577u
#define QUEUE_ID_FREE_NUM 8

#define DMA_CNT 5

static int g_plog_sge_size[PLOG_SGE_SIZE_ARRAY_SIZE] = { URPC_EXT_HEADER_SIZE, SGE_SIZE };

extern queue_ops_t g_urpc_send_recv_ops;
static urma_device_t dev = {0};

static urma_status_t urma_query_device_mock(urma_device_t *dev, urma_device_attr_t *dev_attr)
{
    dev_attr->dev_cap.max_msg_size = MAX_MSG_SIZE;
    return URMA_SUCCESS;
}

static urma_status_t urma_query_jetty_mock(urma_jetty_t *jetty, urma_jetty_cfg_t *cfg, urma_jetty_attr_t *attr)
{
    attr->state = URMA_JETTY_STATE_READY;
    return URMA_SUCCESS;
}

static urma_status_t urma_query_jfr_mock(urma_jfr_t *jfr, urma_jfr_cfg_t *cfg, urma_jfr_attr_t *attr)
{
    attr->state = URMA_JFR_STATE_READY;
    return URMA_SUCCESS;
}

class queue_test : public ::testing::Test {
public:
    // SetUP 在每一个 TEST_F 测试开始前执行一次
    void SetUp() override
    {
        dev.type = URMA_TRANSPORT_UB;
        static urma_eid_info_t eid_info = {0};
        (void)urma_str_to_eid("127.0.0.1", &eid_info.eid);
        static uint32_t eid_num = 1;
        static urma_context_t urma_ctx = {0};
        urma_ctx.dev = &dev;
        static urma_device_attr_t dev_attr = {0};
        dev_attr.dev_cap.max_msg_size = 65536;
        MOCKER(urma_init).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_user_ctl).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_query_device)
            .stubs()
            .with(any(), outBoundP((urma_device_attr_t *)&dev_attr))
            .will(returnValue(URMA_SUCCESS));
        MOCKER(urma_get_device_by_name).stubs().will(returnValue(&dev));
        MOCKER(urma_create_context).stubs().will(returnValue(&urma_ctx));
        urma_notifier_t notifier;
        MOCKER(urma_create_notifier).stubs().will(returnValue(&notifier));
        MOCKER(urma_delete_notifier).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_free_eid_list).stubs().will(ignoreReturnValue());
        MOCKER(urma_get_eid_list)
            .stubs()
            .with(any(), outBoundP((uint32_t *)&eid_num, sizeof(eid_num)))
            .will(returnValue(&eid_info));

        static urma_jfr_t jfr = {0};
        static urma_jfc_t jfc = {0};
        static urma_jfce_t jfce = {0};
        static urma_jetty_t jetty = {0};
        static urma_jfr_cfg_t jfr_cfg = {0};
        jetty.jetty_cfg.jfr_cfg = &jfr_cfg;
        MOCKER(urma_create_jfr).stubs().will(returnValue(&jfr));
        MOCKER(urma_delete_jfr).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jfc).stubs().will(returnValue(&jfc));
        MOCKER(urma_delete_jfc).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jetty).stubs().will(returnValue(&jetty));
        MOCKER(urma_delete_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jfce).stubs().will(returnValue(&jfce));
        MOCKER(urma_delete_jfce).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_query_jetty).stubs().will(invoke(urma_query_jetty_mock));
        MOCKER(urma_modify_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_poll_jfc).stubs().will(returnValue(0));

        MOCKER(urma_modify_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_modify_jfr).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_query_jfr).stubs().will(invoke(urma_query_jfr_mock));

        static uint32_t qid = 0;
        MOCKER(queue_id_allocator_alloc)
            .stubs()
            .with(outBoundP((uint32_t *)&qid, sizeof(qid)))
            .will(returnValue(URPC_SUCCESS));
        MOCKER(queue_id_allocator_free).stubs().will(ignoreReturnValue());

        urpc_trans_info_t cfg = {.trans_mode = URPC_TRANS_MODE_IP, .assign_mode = DEV_ASSIGN_MODE_DEV,};
        (void)snprintf(cfg.dev.dev_name, URPC_DEV_NAME_SIZE, "%s", "lo");
        provider_flag_t flag = {0};
        int ret = provider_init(1, &cfg, flag);
        ASSERT_EQ(ret, URPC_SUCCESS);
        urpc_state_set(URPC_STATE_INIT);
    }

    // TearDown 在每一个 TEST_F 测试完成后执行一次
    void TearDown() override
    {
        MOCKER(urma_delete_context).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_uninit).stubs().will(returnValue(URMA_SUCCESS));
        provider_uninit();

        GlobalMockObject::verify();
    }

    // SetUpTestCase 在所有 TEST_F 测试开始前执行一次
    static void SetUpTestCase()
    {}

    // TearDownTestCase 在所有 TEST_F 测试完成后执行一次
    static void TearDownTestCase()
    {}
};

TEST(queue_not_init_test, send_recv_create_local_queue_without_init)
{
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH;
    queue_cfg.rx_buf_size = DEFAULT_MSG_SIZE;
    queue_cfg.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    // 底下没有urma初始化，预期为空
    ASSERT_EQ(qh, (uint64_t)0);
}

TEST(queue_not_init_test, send_recv_advise_queue_empty)
{
    // 程序不coredump就是成功
    g_urpc_send_recv_ops.bind_queue(NULL, NULL, NULL);
    ASSERT_EQ(0, 0);
    g_urpc_send_recv_ops.unbind_queue(NULL, NULL);
    ASSERT_EQ(0, 0);
}

TEST(queue_not_init_test, send_recv_init_dev_by_ip_addr)
{
    urma_device_t dev = {0};
    dev.type = URMA_TRANSPORT_UB;
    urma_device_t *device_list = &dev;
    int device_num = 1;
    urma_eid_info_t eid_info = {0};
    uint32_t eid_num = 1;
    urma_context_t urma_ctx = {0};
    urma_ctx.dev = &dev;
    MOCKER(urma_init).stubs().will(returnValue(URMA_SUCCESS));
    MOCKER(urma_user_ctl).stubs().will(returnValue(URMA_SUCCESS));
    MOCKER(urma_query_device).stubs().will(invoke(urma_query_device_mock));
    MOCKER(urma_get_device_by_name).stubs().will(returnValue(&dev));
    MOCKER(urma_create_context).stubs().will(returnValue(&urma_ctx));
    MOCKER(urma_free_eid_list).stubs().will(ignoreReturnValue());
    MOCKER(urma_free_device_list).stubs().will(ignoreReturnValue());
    MOCKER(urma_delete_context).stubs().will(returnValue(URMA_SUCCESS));
    MOCKER(urma_uninit).stubs().will(returnValue(URMA_SUCCESS));
    MOCKER(urma_get_eid_list)
        .stubs()
        .with(any(), outBoundP((uint32_t *)&eid_num, sizeof(eid_num)))
        .will(returnValue(&eid_info));
    MOCKER(urma_get_device_list)
        .stubs()
        .with(outBoundP((int *)&device_num, sizeof(device_num)))
        .will(returnValue(&device_list));

    EXPECT_EQ(urma_str_to_eid("127.0.0.1", &eid_info.eid), 0);
    provider_flag_t flag = {0};

    urpc_trans_info_t dp_cfg = {.trans_mode = URPC_TRANS_MODE_IP, .assign_mode = DEV_ASSIGN_MODE_IPV4,};
    (void)snprintf(dp_cfg.ipv4.ip_addr, URPC_IPV4_SIZE, "%s", "127.0.0.1");
    dp_cfg.trans_mode = (enum urpc_trans_mode)10;
    EXPECT_EQ(provider_init(1, &dp_cfg, flag), URPC_FAIL);

    dp_cfg.trans_mode = URPC_TRANS_MODE_IP;
    EXPECT_EQ(provider_init(1, &dp_cfg, flag), URPC_SUCCESS);

    provider_uninit();

    GlobalMockObject::verify();
}

TEST_F(queue_test, send_recv_create_local_queue_real)
{
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag =
        QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_PRIORITY;
    queue_cfg.rx_buf_size = DEFAULT_MSG_SIZE;
    queue_cfg.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg.priority = DEFAULT_PRIORITY;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);

    urpc_qcfg_get_t get_cfg;
    (void)urpc_queue_cfg_get(qh, &get_cfg);
    ASSERT_EQ(get_cfg.priority, DEFAULT_PRIORITY);

    urpc_qcfg_set_t set_cfg;
    set_cfg.set_flag = QCFG_SET_FLAG_PRIORITY;
    int ret = urpc_queue_cfg_set(qh, &set_cfg);
    ASSERT_NE(ret, URPC_SUCCESS);

    ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
}

TEST_F(queue_test, send_recv_shared_rq_and_cq)
{
    dev.type = URMA_TRANSPORT_UB;
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH |
                            QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_MAX_RX_SGE;
    queue_cfg.rx_buf_size = SHARED_JFR_Q1_RX_BUF_SIZE;
    queue_cfg.rx_depth = SHARED_JFR_Q1_RX_DEPTH;
    queue_cfg.max_rx_sge = SHARED_JFR_Q1_MAX_RX_SGE;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);

    urpc_qcfg_create_t queue_cfg1 = {0};
    queue_cfg1.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH |
                             QCREATE_FLAG_QH_SHARE_RQ;
    queue_cfg1.rx_buf_size = SHARED_JFR_Q2_RX_BUF_SIZE;
    queue_cfg1.rx_depth = SHARED_JFR_Q2_RX_DEPTH;
    queue_cfg.max_rx_sge = SHARED_JFR_Q2_MAX_RX_SGE;
    queue_cfg1.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg1.urpc_qh_share_rq = qh;
    uint64_t qh1 = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg1);
    ASSERT_NE(qh1, (uint64_t)0);

    urpc_qcfg_get_t get_cfg;
    (void)urpc_queue_cfg_get(qh, &get_cfg);
    ASSERT_EQ(get_cfg.rx_buf_size, (uint32_t)SHARED_JFR_Q1_RX_BUF_SIZE);
    ASSERT_EQ(get_cfg.rx_depth, (uint32_t)SHARED_JFR_Q1_RX_DEPTH);
    ASSERT_EQ(get_cfg.max_rx_sge, SHARED_JFR_Q1_MAX_RX_SGE);

    urpc_qcfg_get_t get_cfg1;
    (void)urpc_queue_cfg_get(qh1, &get_cfg1);
    ASSERT_EQ(get_cfg1.rx_buf_size, (uint32_t)SHARED_JFR_Q1_RX_BUF_SIZE);
    ASSERT_EQ(get_cfg1.rx_depth, (uint32_t)SHARED_JFR_Q1_RX_DEPTH);
    ASSERT_EQ(get_cfg1.max_rx_sge, SHARED_JFR_Q1_MAX_RX_SGE);

    int ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = urpc_queue_destroy(qh1);
    ASSERT_EQ(ret, URPC_SUCCESS);
    dev.type = URMA_TRANSPORT_UB;
}

TEST_F(queue_test, send_recv_create_shared_txrx_cq)
{
    dev.type = URMA_TRANSPORT_UB;
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH |
                            QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_MAX_RX_SGE | QCREATE_FLAG_TX_CQ_DEPTH;
    queue_cfg.rx_buf_size = SHARED_JFR_Q1_RX_BUF_SIZE;
    queue_cfg.rx_depth = SHARED_JFR_Q1_RX_DEPTH;
    queue_cfg.max_rx_sge = SHARED_JFR_Q1_MAX_RX_SGE;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg.tx_cq_depth = 2 * (DEFAULT_TX_DEPTH + 1);
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);

    urpc_qcfg_get_t get_cfg;
    (void)urpc_queue_cfg_get(qh, &get_cfg);
    ASSERT_EQ(get_cfg.rx_buf_size, (uint32_t)SHARED_JFR_Q1_RX_BUF_SIZE);
    ASSERT_EQ(get_cfg.rx_depth, (uint32_t)SHARED_JFR_Q1_RX_DEPTH);
    ASSERT_EQ(get_cfg.max_rx_sge, SHARED_JFR_Q1_MAX_RX_SGE);
    ASSERT_EQ(get_cfg.tx_cq_depth, 2 * (DEFAULT_TX_DEPTH + 1));
    ASSERT_EQ(get_cfg.rx_cq_depth, queue_cfg.rx_depth);

    urpc_qcfg_create_t queue_cfg1 = {0};
    queue_cfg1.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH |
                             QCREATE_FLAG_MAX_RX_SGE | QCREATE_FLAG_QH_SHARE_TX_CQ;
    queue_cfg1.rx_buf_size = SHARED_JFC_RX_BUF_SIZE;
    queue_cfg1.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg1.max_rx_sge = SHARED_JFC_MAX_RX_SGE;
    queue_cfg1.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg1.urpc_qh_share_tx_cq = qh;
    uint64_t qh1 = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg1);
    ASSERT_NE(qh1, (uint64_t)0);

    urpc_qcfg_get_t get_cfg1;
    (void)urpc_queue_cfg_get(qh1, &get_cfg1);
    ASSERT_EQ(get_cfg1.rx_buf_size, (uint32_t)SHARED_JFC_RX_BUF_SIZE);
    ASSERT_EQ(get_cfg1.rx_depth, (uint32_t)DEFAULT_RX_DEPTH);
    ASSERT_EQ(get_cfg1.max_rx_sge, SHARED_JFC_MAX_RX_SGE);
    ASSERT_EQ(get_cfg1.tx_cq_depth, 2 * (DEFAULT_TX_DEPTH + 1));
    ASSERT_EQ(get_cfg1.rx_cq_depth, queue_cfg1.rx_depth);

    int ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = urpc_queue_destroy(qh1);
    ASSERT_EQ(ret, URPC_SUCCESS);
    dev.type = URMA_TRANSPORT_UB;
}

TEST_F(queue_test, send_recv_create_shared_txrx_cq_fail)
{
    dev.type = URMA_TRANSPORT_UB;
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_MODE|
                            QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_MAX_RX_SGE | QCREATE_FLAG_TX_CQ_DEPTH;
    queue_cfg.rx_buf_size = SHARED_JFR_Q1_RX_BUF_SIZE;
    queue_cfg.rx_depth = SHARED_JFR_Q1_RX_DEPTH;
    queue_cfg.max_rx_sge = SHARED_JFR_Q1_MAX_RX_SGE;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg.tx_cq_depth = 3 * (DEFAULT_TX_DEPTH + 1);
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    uint64_t qh1 = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);
    ASSERT_NE(qh1, (uint64_t)0);

    urpc_qcfg_create_t queue_cfg1 = {0};
    queue_cfg1.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH |
                             QCREATE_FLAG_MAX_RX_SGE | QCREATE_FLAG_QH_SHARE_TX_CQ |
                             QCREATE_FLAG_QH_SHARE_RQ | QCREATE_FLAG_MODE;
    queue_cfg1.rx_buf_size = SHARED_JFC_RX_BUF_SIZE;
    queue_cfg1.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg1.max_rx_sge = SHARED_JFC_MAX_RX_SGE;
    queue_cfg1.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg1.urpc_qh_share_tx_cq = qh;
    uint64_t qh2 = urpc_queue_create(QUEUE_TRANS_MODE_JETTY_DISORDER, &queue_cfg1);
    ASSERT_EQ(qh2, (uint64_t)0);
    queue_cfg.mode = QUEUE_MODE_INTERRUPT;
    queue_cfg1.urpc_qh_share_rq = qh;
    qh2 = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg1);
    ASSERT_NE(qh2, (uint64_t)0);

    int ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = urpc_queue_destroy(qh1);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = urpc_queue_destroy(qh2);
    ASSERT_EQ(ret, URPC_SUCCESS);
    dev.type = URMA_TRANSPORT_UB;
}

TEST_F(queue_test, send_recv_queue_ext_cfg_get)
{
    int ret;
    dev.type = URMA_TRANSPORT_UB;
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag =
        QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_PRIORITY;
    queue_cfg.rx_buf_size = DEFAULT_MSG_SIZE;
    queue_cfg.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg.priority = DEFAULT_PRIORITY;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);

    urpc_ext_q_cfg_get_t ext_get_cfg;

    ret = urpc_queue_ext_cfg_get(0, &ext_get_cfg);
    ASSERT_EQ(ret, URPC_FAIL);

    ret = urpc_queue_ext_cfg_get(qh, 0);
    ASSERT_EQ(ret, URPC_FAIL);

    ret = urpc_queue_ext_cfg_get(qh, &ext_get_cfg);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ASSERT_EQ(ext_get_cfg.trans_mode, QUEUE_TRANS_MODE_JETTY);

    ASSERT_EQ(ext_get_cfg.local_mode_jetty.local_ctx_cnt, (uint32_t)1);
    ASSERT_EQ(ext_get_cfg.local_mode_jetty.queue_info[0].local_ctx_id, (uint32_t)0);
    ASSERT_EQ(ext_get_cfg.local_mode_jetty.queue_info[0].jetty_id, (uint32_t)0);
    ASSERT_EQ(ext_get_cfg.local_mode_jetty.queue_info[0].jetty_token, (uint32_t)0);

    ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);

    dev.type = URMA_TRANSPORT_UB;
}

TEST_F(queue_test, test_list_queue_local)
{
    int ret = 0;
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag =
        QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_PRIORITY;
    queue_cfg.rx_buf_size = DEFAULT_MSG_SIZE;
    queue_cfg.rx_depth = DEFAULT_RX_DEPTH;
    queue_cfg.tx_depth = DEFAULT_TX_DEPTH;
    queue_cfg.priority = DEFAULT_PRIORITY;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    ASSERT_NE(qh, (uint64_t)0);

    char *output = NULL;
    uint32_t output_size = 0;
    ASSERT_EQ(get_queue_trans_info(&output, &output_size), 0);
    ASSERT_NE(output_size, (uint32_t)0);
    ASSERT_EQ(output != NULL, true);

    queue_trans_info_t *trans_info = (queue_trans_info_t *)output;

    ASSERT_EQ(trans_info->flag.is_remote, 0);
    ASSERT_EQ(trans_info->trans_spec_cnt, (uint32_t)1);

    urpc_dbuf_free(output);

    ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
}

TEST_F(queue_test, test_queue_info_get)
{
    urpc_qcfg_create_t queue_cfg = {0};
    queue_cfg.create_flag = QCREATE_FLAG_RX_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH |
        QCREATE_FLAG_CUSTOM_FLAG;
    queue_cfg.rx_buf_size = 4096;
    queue_cfg.rx_depth = 16;
    queue_cfg.tx_depth = 16;
    queue_cfg.custom_flag = QALLOCA_NORMAL_SIZE_FLAG;
    uint64_t qh = urpc_queue_create(QUEUE_TRANS_MODE_JETTY, &queue_cfg);
    queue_local_t *lq = (queue_local_t *)qh;
    uint16_t qid = lq->qid;
    char *output = NULL;
    uint32_t output_size = 0;
    int ret = queue_info_get(qid, &output, &output_size);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ASSERT_EQ(output != NULL, true);
    ASSERT_EQ(output_size, sizeof(queue_trans_info_t) + sizeof(queue_trans_resource_spec_t));

    queue_trans_info_t *qti = (queue_trans_info_t *)output;
    ASSERT_EQ(qti->trans_spec_cnt, 1);
    ASSERT_EQ(qti->custom_flag, QALLOCA_NORMAL_SIZE_FLAG);

    urpc_dbuf_free(output);
    ret = urpc_queue_destroy(qh);
    ASSERT_EQ(ret, URPC_SUCCESS);
}

typedef struct test_read_cache_args {
    uint32_t list_size;
    uint32_t concurrent_cnt;
    read_cache_list_t *rcache_list;
    std::atomic<int> ready_cnt;
} test_read_cache_args_t;

void sync_multi_thread(std::atomic<int> &ready_cnt, uint32_t concurrent_cnt)
{
    ready_cnt.fetch_add(1);
    while (ready_cnt.load() != (int)concurrent_cnt) {
    }
}

void *test_read_cache_concurrent_push_back_callback(void *args)
{
    test_read_cache_args_t *cache_args = (test_read_cache_args_t *)args;
    uint32_t push_back_cnt = cache_args->list_size / cache_args->concurrent_cnt;
    sync_multi_thread(cache_args->ready_cnt, cache_args->concurrent_cnt);
    for (uint32_t i = 0; i < push_back_cnt; i++) {
        read_cache_t *read_cache = (read_cache_t *)calloc(1, sizeof(read_cache_t));
        EXPECT_TRUE(read_cache != nullptr);
        int ret = queue_read_cache_list_push_back(cache_args->rcache_list, read_cache);
        EXPECT_EQ(ret, URPC_SUCCESS);
    }
    return nullptr;
}

void *test_read_cache_list_upper_limit_callback(void *args)
{
    test_read_cache_args_t *cache_args = (test_read_cache_args_t *)args;
    read_cache_t *read_cache = (read_cache_t *)calloc(1, sizeof(read_cache_t));
    EXPECT_TRUE(read_cache != nullptr);
    sync_multi_thread(cache_args->ready_cnt, cache_args->concurrent_cnt);
    int ret = queue_read_cache_list_push_back(cache_args->rcache_list, read_cache);
    EXPECT_EQ(ret, URPC_FAIL);
    queue_read_cache_list_push_front(cache_args->rcache_list, read_cache);
    return nullptr;
}

void *test_read_cache_concurrent_pop_front_callback(void *args)
{
    test_read_cache_args_t *cache_args = (test_read_cache_args_t *)args;
    uint32_t pop_front_cnt = (cache_args->list_size / cache_args->concurrent_cnt) + 1;
    sync_multi_thread(cache_args->ready_cnt, cache_args->concurrent_cnt);
    for (uint32_t i = 0; i < pop_front_cnt; i++) {
        read_cache_t *read_cache = queue_read_cache_list_pop_front(cache_args->rcache_list);
        EXPECT_TRUE(read_cache != nullptr);
        free(read_cache);
    }
    return nullptr;
}

void test_read_cache_concurrent_push_back(test_read_cache_args_t *cache_args)
{
    pthread_t thd[cache_args->concurrent_cnt];
    cache_args->ready_cnt.store(0);
    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_create(&thd[i], nullptr, test_read_cache_concurrent_push_back_callback, cache_args);
    }

    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_join(thd[i], nullptr);
    }

    ASSERT_EQ(queue_read_cache_list_size(cache_args->rcache_list), cache_args->list_size);
}

void test_read_cache_list_upper_limit(test_read_cache_args_t *cache_args)
{
    pthread_t thd[cache_args->concurrent_cnt];
    cache_args->ready_cnt.store(0);
    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_create(&thd[i], nullptr, test_read_cache_list_upper_limit_callback, cache_args);
    }

    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_join(thd[i], nullptr);
    }

    ASSERT_EQ(queue_read_cache_list_size(cache_args->rcache_list), cache_args->list_size + cache_args->concurrent_cnt);
}

void test_read_cache_concurrent_pop_front(test_read_cache_args_t *cache_args)
{
    pthread_t thd[cache_args->concurrent_cnt];
    cache_args->ready_cnt.store(0);
    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_create(&thd[i], nullptr, test_read_cache_concurrent_pop_front_callback, cache_args);
    }

    for (uint32_t i = 0; i < cache_args->concurrent_cnt; i++) {
        (void)pthread_join(thd[i], nullptr);
    }

    ASSERT_EQ(queue_read_cache_list_size(cache_args->rcache_list), (uint32_t)0);
}

TEST_F(queue_test, test_read_cache_list)
{
    read_cache_list_t rcache_list;

    queue_read_cache_list_init(&rcache_list, 0);

    test_read_cache_args_t cache_args = {0};
    cache_args.concurrent_cnt = READ_CACHE_LIST_TEST_CONCURRENT_CNT;
    cache_args.list_size = DEFAULT_READ_CACHE_LIST_DEPTH;
    cache_args.rcache_list = &rcache_list;
    cache_args.ready_cnt.store(0);

    test_read_cache_concurrent_push_back(&cache_args);

    test_read_cache_list_upper_limit(&cache_args);

    test_read_cache_concurrent_pop_front(&cache_args);

    queue_read_cache_list_uninit(&rcache_list);
}

mem_handle_t *g_plog_allocator_mem_handle = nullptr;
static std::random_device g_random_device;

static int urpc_plog_allocator_get_raw_buf(urpc_sge_t *sge, uint64_t total_size, urpc_allocator_option_t *option)
{
    sge->length = SGE_SIZE;
    sge->flag = 0;
    sge->addr = (uint64_t)(uintptr_t)calloc(1, SGE_SIZE);
    if (sge->addr == 0) {
        return URPC_FAIL;
    }
    sge->mem_h = (uint64_t)(uintptr_t)g_plog_allocator_mem_handle;

    return URPC_SUCCESS;
}

static int urpc_plog_allocator_put(urpc_sge_t *sge, uint32_t num, urpc_allocator_option_t *option)
{
    for (uint32_t i = 0; i < num; i++) {
        free((void *)(uintptr_t)sge[i].addr);
    }

    free(sge);

    return URPC_SUCCESS;
}

static int urpc_plog_allocator_put_raw_buf(urpc_sge_t *sge, urpc_allocator_option_t *option)
{
    if (sge == NULL || sge[0].addr == 0) {
        return URPC_FAIL;
    }

    free((void *)(uintptr_t)sge->addr);

    return URPC_SUCCESS;
}

static int urpc_plog_allocator_get(urpc_sge_t **sge, uint32_t *num, uint64_t total_size, urpc_allocator_option_t *option)
{
    uint32_t i = 0;
    urpc_sge_t *sge_ = (urpc_sge_t *)malloc(sizeof(urpc_sge_t) * PLOG_SGE_SIZE_ARRAY_SIZE);
    if (sge_ == NULL) {
        return URPC_FAIL;
    }

    for (i = 0; i < PLOG_SGE_SIZE_ARRAY_SIZE; i++) {
        sge_[i].length = g_plog_sge_size[i];
        sge_[i].flag = 0;
        sge_[i].addr = (uint64_t)(uintptr_t)calloc(1, sge_[i].length);
        if (sge_[i].addr == 0) {
            goto ERROR;
        }
        sge_[i].mem_h = (uint64_t)(uintptr_t)g_plog_allocator_mem_handle;
    }

    *num = (int)PLOG_SGE_SIZE_ARRAY_SIZE;
    *sge = sge_;

    return URPC_SUCCESS;

ERROR:
    for (uint32_t j = 0; j < i; j++) {
        free((void *)(uintptr_t)sge_[j].addr);
    }

    free(sge_);

    return URPC_FAIL;
}

static int urpc_plog_allocator_get_sges(urpc_sge_t **sge, uint32_t num, urpc_allocator_option_t *option)
{
    if (num == 0) {
        return URPC_FAIL;
    }

    urpc_sge_t *tmp_sge = (urpc_sge_t *)calloc(num, sizeof(urpc_sge_t));
    if (tmp_sge == NULL) {
        return URPC_FAIL;
    }

    for (uint32_t i = 0; i < num; i++) {
        tmp_sge[i].flag = SGE_FLAG_NO_MEM;
    }

    *sge = tmp_sge;

    return URPC_SUCCESS;
}

static int urpc_plog_allocator_put_sges(urpc_sge_t *sge, urpc_allocator_option_t *option)
{
    if (sge == NULL) {
        return URPC_FAIL;
    }

    free(sge);
    return URPC_SUCCESS;
}

static urpc_allocator_t g_plog_allocator = {
    .get = urpc_plog_allocator_get,
    .put = urpc_plog_allocator_put,
    .get_raw_buf = urpc_plog_allocator_get_raw_buf,
    .put_raw_buf = urpc_plog_allocator_put_raw_buf,
    .get_sges = urpc_plog_allocator_get_sges,
    .put_sges = urpc_plog_allocator_put_sges,
};

send_recv_queue_local_t *g_test_send_recv_queue_read_cache_list_queue = nullptr;
class WrInfo {
public:
    WrInfo(urma_sge_t *sge, uint32_t num_sge, uint64_t user_ctx) : m_user_ctx(user_ctx), m_num_sge(num_sge)
    {
        m_timestamp = get_timestamp_ns();
        uint32_t mem_size = num_sge * sizeof(urma_sge_t);
        memcpy((&m_sge, sge, mem_size);
        for (uint32_t i = 0; i < m_num_sge; i++) {
            m_sge_total_len += m_sge[i].len;
        }
    }

    uint64_t m_timestamp;
    uint64_t m_user_ctx;
    urma_sge_t m_sge[PLOG_SGE_SIZE_ARRAY_SIZE];
    uint32_t m_num_sge;
    uint32_t m_sge_total_len = 0;
};

static pthread_spinlock_t g_tx_lock;
static pthread_spinlock_t g_rx_lock;
static std::deque<WrInfo> g_test_send_recv_queue_read_cache_list_rx_vec;
static std::deque<WrInfo> g_test_send_recv_queue_read_cache_list_tx_vec;
static uint32_t g_simulate_post_read_duration;
static bool g_simulate_flush_jetty;
static bool g_simulate_flush_jfr;
static uint32_t g_total_rx_cr_cnt;
static std::atomic<uint32_t> g_req_id;

static uint32_t fill_plog_read_proto_info(WrInfo &wr)
{
    uint32_t req_id = g_req_id.fetch_add(1);
    urpc_req_head_t *req_head = (urpc_req_head_t *)wr.m_sge[0].addr;
    urpc_req_fill_basic_info(req_head, 0, 0);
    urpc_req_fill_req_info_without_dma(req_head, 0, wr.m_sge_total_len, req_id, FUNC_DEF_PLOG);
    urpc_plog_req_exthdr_t *exthdr =
        (urpc_plog_req_exthdr_t *)((uintptr_t)(wr.m_sge[0].addr + sizeof(urpc_req_head_t)));
    uint32_t dma_cnt = wr.m_sge[1].len / PLOG_DMA_INFO_SIZE;

    urpc_plog_req_exthdr_fill_version(exthdr, URPC_EXTHDR_PLOG_VERSION);
    urpc_plog_req_exthdr_fill_data_trans_mode(exthdr, MODE_SEND_READ);
    urpc_plog_req_exthdr_fill_user_hdr_offset(exthdr, PLOG_READ_HEADER_ROOM_SIZE - sizeof(urpc_req_head_t));
    urpc_plog_req_exthdr_fill_user_data_offset(exthdr, PLOG_READ_HEADER_TOTAL_SIZE - sizeof(urpc_req_head_t));
    urpc_plog_req_exthdr_fill_flow_ctrl_flag(exthdr, 0);
    urpc_plog_req_exthdr_fill_data_zone_dma_cnt(exthdr, dma_cnt);
    urpc_plog_req_exthdr_fill_early_rsp(exthdr, false);

    urpc_plog_dma_t *dma = (urpc_plog_dma_t *)wr.m_sge[1].addr;
    for (uint32_t i = 0; i < dma_cnt; i++) {
        urpc_plog_dma_fill_size(dma + i, SGE_SIZE);
        urpc_plog_dma_fill_address(dma + i, (uint64_t)(uintptr_t)dma);
    }
    return req_id;
}

static int urma_poll_jfr_jfc_mock(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
    pthread_spin_lock(&g_rx_lock);
    if (!g_simulate_flush_jfr && g_total_rx_cr_cnt <= 0) {
        pthread_spin_unlock(&g_rx_lock);
        return 0;
    }

    int i = 0;
    while (i < cr_cnt && !g_test_send_recv_queue_read_cache_list_rx_vec.empty() && (g_simulate_flush_jfr || g_total_rx_cr_cnt > 0)) {
        auto wr = g_test_send_recv_queue_read_cache_list_rx_vec.front();
        g_test_send_recv_queue_read_cache_list_rx_vec.pop_front();
        if (!g_simulate_flush_jfr) {
            uint32_t req_id = fill_plog_read_proto_info(wr);
            cr[i].completion_len += wr.m_sge_total_len;
            cr[i].user_ctx = wr.m_user_ctx;
            cr[i].status = URMA_CR_SUCCESS;
            g_total_rx_cr_cnt--;
        } else {
            cr[i].completion_len += wr.m_sge_total_len;
            cr[i].user_ctx = wr.m_user_ctx;
            cr[i].status = URMA_CR_WR_FLUSH_ERR;
        }
        i++;
    }

    pthread_spin_unlock(&g_rx_lock);
    return i;
}

static int urma_poll_jfs_jfc_mock(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
    int i = 0;
    pthread_spin_lock(&g_tx_lock);
    for (; i < cr_cnt && !g_test_send_recv_queue_read_cache_list_tx_vec.empty(); i++) {
        auto wr = g_test_send_recv_queue_read_cache_list_tx_vec.front();
        uint64_t timestamp = get_timestamp_ns();
        if (!g_simulate_flush_jetty && (timestamp - wr.m_timestamp < g_simulate_post_read_duration)) {
            pthread_spin_unlock(&g_tx_lock);
            return i;
        }
        g_test_send_recv_queue_read_cache_list_tx_vec.pop_front();
        cr[i].completion_len += wr.m_sge_total_len;
        cr[i].user_ctx = wr.m_user_ctx;
        cr[i].status = URMA_CR_SUCCESS;
    }

    if (g_simulate_flush_jetty && i < cr_cnt && g_test_send_recv_queue_read_cache_list_rx_vec.empty()) {
        cr[i].status = URMA_CR_WR_SUSPEND_DONE;
    }

    pthread_spin_unlock(&g_tx_lock);
    return i;
}

static int urma_poll_jfc_mock(urma_jfc_t *jfc, int cr_cnt, urma_cr_t *cr)
{
    if (jfc == g_test_send_recv_queue_read_cache_list_queue->jfr_jfc) {
        return urma_poll_jfr_jfc_mock(jfc, cr_cnt, cr);
    }

    return urma_poll_jfs_jfc_mock(jfc, cr_cnt, cr);
}

static urma_status_t urma_post_jetty_send_wr_mock(urma_jetty_t *jetty, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    pthread_spin_lock(&g_tx_lock);
    if (g_test_send_recv_queue_read_cache_list_tx_vec.size() >=
        g_test_send_recv_queue_read_cache_list_queue->local_q.cfg.tx_depth) {
        pthread_spin_unlock(&g_tx_lock);
        return URMA_EAGAIN;
    }

    g_test_send_recv_queue_read_cache_list_tx_vec.emplace_back(wr->send.src.sge, wr->send.src.num_sge, wr->user_ctx);
    pthread_spin_unlock(&g_tx_lock);
    return URMA_SUCCESS;
}

static urma_status_t urma_post_jetty_recv_wr_mock(urma_jetty_t *jetty, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad_wr)
{
    pthread_spin_lock(&g_rx_lock);
    g_test_send_recv_queue_read_cache_list_rx_vec.emplace_back(wr->src.sge, wr->src.num_sge, wr->user_ctx);
    pthread_spin_unlock(&g_rx_lock);
    return URMA_SUCCESS;
}

static urma_status_t urma_modify_jetty_mock(urma_jetty_t *jetty, urma_jetty_attr_t *attr)
{
    if (attr->state == URMA_JETTY_STATE_SUSPENDED || attr->state == URMA_JETTY_STATE_ERROR) {
        pthread_spin_lock(&g_tx_lock);
        g_simulate_flush_jetty = true;
        pthread_spin_unlock(&g_tx_lock);
    }

    return URMA_SUCCESS;
}

static urma_status_t urma_modify_jfr_mock(urma_jfr_t *jfr, urma_jfr_attr_t *attr)
{
    if (attr->state == URMA_JFR_STATE_ERROR) {
        pthread_spin_lock(&g_rx_lock);
        g_simulate_flush_jfr = true;
        pthread_spin_unlock(&g_rx_lock);
    }

    return URMA_SUCCESS;
}
