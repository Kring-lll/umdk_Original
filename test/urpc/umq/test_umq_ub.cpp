/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: umq ub test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include "umq_api.h"
#include "urma_api.h"
#include "umq_ub_private.h"
#include "umq_qbuf_pool.h"
#include "util_id_generator.h"

#define TEST_DEV_NUM 1
static const uint32_t EXAMPLE_BUFFER_SIZE = 8192;
static const uint32_t EXAMPLE_DEPTH = 128;
static urma_device_t dev = {0};
static urma_device_t g_dev_list[TEST_DEV_NUM];
static urma_device_t *g_dev_ptr_list[TEST_DEV_NUM];

urma_device_t **urma_get_device_list_mock(int *num_devices)
{
    *num_devices = 1;
    (void)snprintf(g_dev_list[0].name, URMA_MAX_NAME, "%s", "bonding_dev_0");
    g_dev_list[0].type = URMA_TRANSPORT_UB;
    g_dev_ptr_list[0] = &g_dev_list[0];
    return (urma_device_t **)&g_dev_ptr_list;
}

class UmqUBTest : public ::testing::Test {
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
        static urma_target_seg_t tseg;
        dev_attr.dev_cap.max_jetty = 65536;
        dev_attr.dev_cap.max_msg_size = 65536;
        dev_attr.dev_cap.max_jfc_depth = 8192;
        dev_attr.dev_cap.max_jfs_depth = 8192;
        dev_attr.dev_cap.max_jfr_depth = 8192;
        dev_attr.dev_cap.max_jfr_sge = 6;
        dev_attr.dev_cap.max_jfs_sge = 6;
        MOCKER(urma_init).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_query_device).stubs()
            .with(mockcpp::any(), outBoundP((urma_device_attr_t *)&dev_attr))
            .will(returnValue(URMA_SUCCESS));
        MOCKER(urma_get_device_by_name).stubs().will(returnValue(&dev));
        MOCKER(urma_create_context).stubs().will(returnValue(&urma_ctx));
        MOCKER(urma_free_eid_list).stubs().will(ignoreReturnValue());
        MOCKER(urma_get_eid_list).stubs()
            .with(mockcpp::any(), outBoundP((uint32_t *)&eid_num, sizeof(eid_num)))
            .will(returnValue(&eid_info));
        MOCKER(urma_get_device_list).stubs().will(invoke(urma_get_device_list_mock));
        MOCKER(urma_register_seg).stubs().will(returnValue(&tseg));
        MOCKER(urma_free_device_list).stubs().will(ignoreReturnValue());

        static urma_jfr_t jfr = {0};
        static urma_jfc_t jfc = {0};
        static urma_jfce_t jfce = {0};
        static urma_jetty_t jetty = {0};
        static urma_jfr_cfg_t jfr_cfg = {0};
        jetty.jetty_cfg.jfr_cfg = &jfr_cfg;
        static urma_target_jetty target_jetty = {0};
        MOCKER(urma_create_jfr).stubs().will(returnValue(&jfr));
        MOCKER(urma_delete_jfr).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jfc).stubs().will(returnValue(&jfc));
        MOCKER(urma_delete_jfc).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jetty).stubs().will(returnValue(&jetty));
        MOCKER(urma_delete_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_create_jfce).stubs().will(returnValue(&jfce));
        MOCKER(urma_delete_jfce).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_modify_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_poll_jfc).stubs().will(returnValue(0));
        MOCKER(urma_import_jetty).stubs().will(returnValue(&target_jetty));
        MOCKER(urma_import_seg).stubs().will(returnValue(&tseg));
        MOCKER(urma_bind_jetty).stubs().will(returnValue(URMA_SUCCESS));
        MOCKER(urma_post_jetty_recv_wr).stubs().will(returnValue(URMA_SUCCESS));
    }

    // TearDown 在每一个 TEST_F 测试完成后执行一次
    void TearDown() override
    {
        GlobalMockObject::verify();
    }

    // SetUpTestCase 在所有 TEST_F 测试开始前执行一次
    static void SetUpTestCase()
    {
    }

    // TearDownTestCase 在所有 TEST_F 测试完成后执行一次
    static void TearDownTestCase()
    {
    }
};

TEST_F(UmqUBTest, test_umq_ub)
{
    umq_init_cfg_t cfg;
    memset(&cfg, 0, sizeof(umq_init_cfg_t));
    int ret;

    cfg.trans_info_num = 1;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
}

TEST_F(UmqUBTest, test_ub_control_plane_success)
{
    umq_init_cfg_t cfg;
    memset(&cfg, 0, sizeof(umq_init_cfg_t));
    int ret;
    uint8_t local_bind_info[512] = {0};
    uint32_t bind_info_size = 512;

    cfg.trans_info_num = 1;
    cfg.feature = UMQ_FEATURE_ENABLE_FLOW_CONTROL;
    cfg.trans_info[0].trans_mode = UMQ_TRANS_MODE_UB;
    cfg.trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    strcpy(cfg.trans_info[0].dev_info.dev.dev_name, "bonding_dev_0");
    cfg.trans_info[0].dev_info.dev.eid_idx = 0;
    umq_create_option_t option = {
        .trans_mode = cfg.trans_info[0].trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE | UMQ_CREATE_FLAG_RX_DEPTH |
            UMQ_CREATE_FLAG_TX_DEPTH | UMQ_CREATE_FLAG_QUEUE_MODE | UMQ_CREATE_FLAG_UMQ_CTX,
        .rx_buf_size = EXAMPLE_BUFFER_SIZE,
        .tx_buf_size = EXAMPLE_BUFFER_SIZE,
        .rx_depth = EXAMPLE_DEPTH,
        .tx_depth = EXAMPLE_DEPTH,
        .mode = (umq_queue_mode_t)1
    };

    ret = umq_init(&cfg);
    ASSERT_EQ(ret, 0);
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    sprintf(option.name, "%s", "server");
    (void)memcpy(&option.dev_info, &cfg.trans_info[0].dev_info, sizeof(umq_dev_assign_t));
    uint64_t umqh = umq_create(&option);
    ASSERT_NE(umqh, 0);
    bind_info_size = umq_bind_info_get(umqh, local_bind_info, bind_info_size);

    option.create_flag |= UMQ_CREATE_FLAG_SHARE_RQ;
    option.create_flag |= UMQ_CREATE_FLAG_SUB_UMQ;
    option.share_rq_umqh = umqh;
    urma_jetty_t jetty1 = {0};
    jetty1.jetty_id.id = 1;
    MOCKER(umq_create_jetty).stubs().will(returnValue(&jetty1));
    uint64_t umqh1 = umq_create(&option);
    ASSERT_NE(umqh1, 0);

    MOCKER(umq_ub_post_rx_inner_impl).stubs().will(returnValue(0));
    ret = umq_bind(umqh1, local_bind_info, bind_info_size);
    ASSERT_EQ(ret, 0);

    ret = umq_unbind(umqh1);
    ASSERT_EQ(ret, 0);
    umq_destroy(umqh1);
    ASSERT_EQ(ret, 0);
    umq_destroy(umqh);
    ASSERT_EQ(ret, 0);
    umq_uninit();
}

TEST_F(UmqUBTest, test_ub_init_failure)
{
    umq_init_cfg_t cfg;
    memset(&cfg, 0, sizeof(umq_init_cfg_t));
    int ret;

    cfg.trans_info_num = 1;
    cfg.feature = 0;
    cfg.trans_info[0].trans_mode = UMQ_TRANS_MODE_UB;
    cfg.trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    strcpy(cfg.trans_info[0].dev_info.dev.dev_name, "bonding_dev_0");
    cfg.trans_info[0].dev_info.dev.eid_idx = 0;

    MOCKER(umq_qbuf_pool_init).stubs().will(returnValue(-1));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    MOCKER(umq_io_buf_malloc).stubs().will(returnValue((void *)NULL));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    cfg.trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_EID;
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
    cfg.trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;

    MOCKER(umq_ub_ctx_imported_info_create).stubs().will(returnValue((remote_imported_tseg_info_t *)NULL));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    MOCKER(urma_init).stubs().will(returnValue(-1));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    MOCKER(umq_ub_id_allocator_init).stubs().will(returnValue(-1));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);

    MOCKER(calloc).stubs().will(returnValue((void *)NULL));
    ret = umq_init(&cfg);
    ASSERT_NE(ret, 0);
}

/**
 * # 将 <your_token> 替换为实际的 Token
 * git remote set-url origin https://<your_token>@atomgit.com/zhaohongrui3/umdk.git
 * # 然后重新操作
 * git push origin main
 */

TEST_F(UmqUBTest, test_umq_ub_dev_functions)
{
    int ret;
    umq_dev_assign_t dev_info = {0};
    char dev_str[UMQ_UB_DEV_STR_LENGTH] = {0};
    umq_ub_raw_dev_t raw_dev = {0};
    umq_eid_t eid = {0};
    (void)urma_str_to_eid("127.0.0.1", &eid.eid);

    // 测试umq_ub_dev_str_get函数 - 设备模式
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    strcpy(dev_info.dev.dev_name, "bonding_dev_0");
    dev_info.dev.eid_idx = 0;
    ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // 测试umq_ub_dev_str_get函数 - EID模式
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_EID;
    dev_info.eid = eid;
    ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // 测试umq_ub_dev_str_get函数 - IPv4模式
    dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_IPV4;
    strcpy(dev_info.ipv4.ip_addr, "192.168.1.1");
    ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_EQ(ret, UMQ_SUCCESS);

    // 测试umq_ub_dev_str_get函数 - 无效模式
    dev_info.assign_mode = (umq_dev_assign_mode_t)999;
    ret = umq_ub_dev_str_get(&dev_info, dev_str, sizeof(dev_str));
    ASSERT_NE(ret, UMQ_SUCCESS);

    // 测试设备信息初始化和获取
    MOCKER(urma_get_device_list).stubs().will(invoke(urma_get_device_list_mock));
    MOCKER(urma_get_eid_list).stubs().with(mockcpp::any(), mockcpp::outBoundP((uint32_t *)mockcpp::any()))
        .will(returnValue((urma_eid_info_t *)NULL));

    ret = umq_ub_dev_info_init();
    ASSERT_EQ(ret, UMQ_SUCCESS);

    int dev_num = umq_ub_dev_num_get();
    ASSERT_EQ(dev_num, 1);

    // 测试设备信息转储
    umq_dev_info_t dev_dump;
    umq_ub_dev_info_dump(UMQ_TRANS_MODE_UB, 1, &dev_dump);
    ASSERT_STREQ(dev_dump.dev_name, "bonding_dev_0");

    // 测试按名称获取设备信息
    ret = umq_ub_dev_info_dump_by_name("bonding_dev_0", UMQ_TRANS_MODE_UB, &dev_dump);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_STREQ(dev_dump.dev_name, "bonding_dev_0");

    // 测试查找不存在的设备
    ret = umq_ub_dev_info_dump_by_name("nonexistent_dev", UMQ_TRANS_MODE_UB, &dev_dump);
    ASSERT_NE(ret, UMQ_SUCCESS);

    // 清理设备信息
    umq_ub_dev_info_uninit();
}

TEST_F(UmqUBTest, test_rx_buf_ctx_list_functions)
{
    rx_buf_ctx_list_t rx_buf_ctx_list = {0};
    uint32_t ctx_num = 10;

    // 测试初始化
    int ret = rx_buf_ctx_list_init(&rx_buf_ctx_list, ctx_num);
    ASSERT_EQ(ret, UMQ_SUCCESS);
    ASSERT_NE(rx_buf_ctx_list.addr, nullptr);

    // 测试刷新
    rx_buf_ctx_t *rx_buf_ctx = queue_rx_buf_ctx_flush(&rx_buf_ctx_list);
    ASSERT_NE(rx_buf_ctx, nullptr);

    // 测试反初始化
    rx_buf_ctx_list_uninit(&rx_buf_ctx_list);
    ASSERT_EQ(rx_buf_ctx_list.addr, nullptr);
}

TEST_F(UmqUBTest, test_umq_ub_bind_info_functions)
{
    ub_queue_t queue = {0};
    umq_ub_bind_info_t bind_info = {0};
    uint8_t bind_info_buf[512] = {0};
    uint32_t bind_info_size = 512;

    // 初始化队列结构
    queue.state = QUEUE_STATE_READY;
    queue.tp_mode = URMA_TRANSPORT_MODE_RAW;
    queue.tp_type = URMA_TP_IB;
    queue.flow_control.enabled = false;

    // 测试绑定信息检查（无效情况）
    int ret = umq_ub_bind_info_check(&queue, &bind_info);
    ASSERT_NE(ret, UMQ_SUCCESS);

    // 测试绑定信息序列化
    ret = umq_ub_bind_info_serialize(&queue, bind_info_buf, bind_info_size);
    ASSERT_GT(ret, 0);

    // 测试绑定信息反序列化
    ret = umq_ub_bind_info_deserialize(bind_info_buf, bind_info_size, &bind_info);
    ASSERT_EQ(ret, UMQ_SUCCESS);
}

