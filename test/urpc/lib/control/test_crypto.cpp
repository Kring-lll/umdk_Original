/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: urpc crypto test
 */

#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"

#include "cp.h"
#include "state.h"
#include "keepalive.h"
#include "urpc_framework_api.h"

#include "urpc_framework_errno.h"

#include "../../../../lib/urpc_lib_ext/lib_ext_plog/datapath_ext/dp_ext.h"
#include "../../../../lib/urpc_lib_ext/lib_ext_general/datapath_ext/dp_ext.h"
#include "crypto.h"

#define URPC_UT_EXT_HEADER_SIZE     256
#define URPC_UT_USR_PAYLOAD_SIZE    4096
#define URPC_UT_HDR_ROOM_SIZE       64

static urpc_log_config_t g_log_cfg;

class crypto_test : public ::testing::Test {
public:
    // SetUP 在每一个 TEST_F 测试开始前执行一次
    void SetUp() override
    {
        g_log_cfg.log_flag = URPC_LOG_FLAG_LEVEL;
        g_log_cfg.level = URPC_LOG_LEVEL_DEBUG;
        ASSERT_EQ(urpc_log_config_set(&g_log_cfg), URPC_SUCCESS);
        urpc_state_set(URPC_STATE_INIT);
    }

    // TearDown 在每一个 TEST_F 测试完成后执行一次
    void TearDown() override
    {
        g_log_cfg.level = URPC_LOG_LEVEL_INFO;
        ASSERT_EQ(urpc_log_config_set(&g_log_cfg), URPC_SUCCESS);
        urpc_ssl_config_t ssl_config = {0};
        EXPECT_EQ(urpc_ssl_config_set(&ssl_config), URPC_SUCCESS);
        GlobalMockObject::verify();
    }

    // SetUpTestCase 在所有 TEST_F 测试开始前执行一次
    static void SetUpTestCase()
    {}

    // TearDownTestCase 在所有 TEST_F 测试完成后执行一次
    static void TearDownTestCase()
    {}
};

static char g_psk_id[10] = "123456";
static char g_psk_key[10] = "ABCDEF";
static char g_tcp_psk_cipher_list[] = "PSK-AES128-GCM-SHA256:PSK-AES256-GCM-SHA384";
static char g_tcp_psk_cipher_suites[] = "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";
urpc_server_channel_info_t g_server_channel = {0};

static unsigned int client_psk_cb_func(void *ssl, const char *hint, char *identity,
                                unsigned int max_identity_len, unsigned char *psk, unsigned int max_psk_len)
{
    if ((strnlen(g_psk_id, max_identity_len) == max_identity_len) || (strnlen(g_psk_key, max_psk_len) == max_psk_len)) {
        printf("psk id or psk key buffer is not sufficient\n");
        return 0;
    }
    strncpy(identity, g_psk_id, max_identity_len);
    memcpy(psk, g_psk_key, strlen(g_psk_key));

    return strnlen(g_psk_key, max_psk_len);
}

static unsigned int server_psk_cb_func(void *ssl, const char *identity, unsigned char *psk, unsigned int max_psk_len)
{
    if (strcmp(g_psk_id, identity) != 0) {
        printf("unknown client's psk id\n");
        return 0;
    }
    if (strnlen(g_psk_key, max_psk_len) == max_psk_len) {
        printf("no enough buffer to copy psk key\n");
        return 0;
    }
    memcpy((psk, max_psk_len, g_psk_key, strlen(g_psk_key));

    return strnlen(g_psk_key, max_psk_len);
}

static void fill_ssl_config(urpc_ssl_config_t *ssl_config)
{
    ssl_config->ssl_mode = SSL_MODE_PSK;
    ssl_config->ssl_flag |= URPC_SSL_FLAG_ENABLE;
    ssl_config->ssl_flag |= URPC_SSL_FLAG_URPC_ENCRYPT_DISABLE;
    ssl_config->ssl_flag |= URPC_SSL_FLAG_SGE_ENCRYPT_DISABLE;
    ssl_config->min_tls_version = URPC_TLS_VERSION_1_2;
    ssl_config->max_tls_version = URPC_TLS_VERSION_1_3;
    ssl_config->psk.cipher_list = g_tcp_psk_cipher_list;
    ssl_config->psk.cipher_suites = g_tcp_psk_cipher_suites;
    ssl_config->psk.server_cb_func = server_psk_cb_func;
    ssl_config->psk.client_cb_func = client_psk_cb_func;
}

TEST_F(crypto_test, TestCryptoSSLInit)
{
    // no cfg
    ASSERT_EQ(urpc_ssl_config_set(NULL), -URPC_ERR_EINVAL);

    // ssl_flag = 0, ssl not enabled
    urpc_ssl_config_t ssl_config = {0};
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), 0);

    // wrong case, hdr disable, payload enable crypto
    fill_ssl_config(&ssl_config);
    ssl_config.ssl_flag = 0;
    ssl_config.ssl_flag |= URPC_SSL_FLAG_ENABLE;
    ssl_config.ssl_flag |= URPC_SSL_FLAG_URPC_ENCRYPT_DISABLE;
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), -URPC_ERR_EINVAL);

    // wrong case, ssl_mode != SSL_MODE_PSK
    fill_ssl_config(&ssl_config);
    ssl_config.ssl_mode = SSL_MODE_MAX;
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), -URPC_ERR_EINVAL);

    // wrong tls version
    fill_ssl_config(&ssl_config);
    ssl_config.max_tls_version = URPC_TLS_VERSION_MAX;
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), -URPC_ERR_EINVAL);

    // wrong cfg, lack of cipher_list
    fill_ssl_config(&ssl_config);
    ssl_config.psk.cipher_list = NULL;
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), -URPC_ERR_EINVAL);

    // wrong cfg, lack of cipher_suites
    fill_ssl_config(&ssl_config);
    ssl_config.psk.cipher_suites = NULL;
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), -URPC_ERR_EINVAL);

    // right cfg, expect success
    fill_ssl_config(&ssl_config);
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), 0);
}

TEST_F(crypto_test, TestCryptoSSLGetFlag)
{
    // ssl_flag = 0, ssl not enabled
    urpc_ssl_config_t ssl_config = {0};
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), 0);

    bool ret = crypto_is_ssl_enabled();
    ASSERT_EQ(ret, false);

    ret = crypto_is_ssl_enabled_lock_free();
    ASSERT_EQ(ret, false);

    ret = crypto_is_dp_ssl_enabled();
    ASSERT_EQ(ret, false);

    ret = crypto_is_dp_ssl_enabled_lock_free();
    ASSERT_EQ(ret, false);
}

TEST_F(crypto_test, TestCryptoSSLGenKeyID)
{
    urpc_ssl_config_t ssl_config = {0};
    fill_ssl_config(&ssl_config);
    ASSERT_EQ(urpc_ssl_config_set(&ssl_config), 0);

    crypto_key_t crypto_key;
    ASSERT_EQ(crypto_ssl_gen_crypto_key(&crypto_key), 0);

    // crypto generate random id, ont
    uint32_t id = 0;
    ASSERT_NE(crypto_gen_rand_channel_id(id), 0);
}

static urpc_server_channel_info_t* get_server_channel_mock(uint32_t urpc_chid, bool is_write)
{
    (void)pthread_rwlock_rdlock(&g_server_channel.rw_lock);
    return &g_server_channel;
}

TEST_F(crypto_test, TestCryptoEncryptDecryptReq)
{
    // init cipher opt
    urpc_cipher_t cipher_opt = {0};
    crypto_key_t crypto_key;
    int ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // prepare data
    char hdr_sge[URPC_UT_EXT_HEADER_SIZE] = {0};
    char payload_sge[URPC_UT_USR_PAYLOAD_SIZE] = {0};
    urpc_sge_t sges[2] = {{.addr = (uint64_t)(uintptr_t)hdr_sge, .length = URPC_UT_EXT_HEADER_SIZE},
                          {.addr = (uint64_t)(uintptr_t)payload_sge, .length = URPC_UT_USR_PAYLOAD_SIZE}};

    size_t base_size = urpc_hdr_size_get(URPC_REQ, 0);
    urpc_plog_req_exthdr_t *exthdr = (urpc_plog_req_exthdr_t *)(uintptr_t)(sges[0].addr + base_size);
    urpc_plog_req_exthdr_fill_user_hdr_offset(exthdr, URPC_UT_HDR_ROOM_SIZE - base_size);

    // test encrypt & decrypt
    ASSERT_EQ(crypto_encrypt_with_plog_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);

    ASSERT_EQ(crypto_decrypt_with_plog_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);

    // test get cipher from server channel
    (void)pthread_rwlock_init(&g_server_channel.rw_lock, NULL);
    g_server_channel.cipher_opt = &cipher_opt;
    MOCKER(server_channel_get_with_rw_lock).stubs().will(invoke(get_server_channel_mock));

    ASSERT_EQ(crypto_encrypt_with_plog_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);

    ASSERT_EQ(crypto_decrypt_with_plog_ext_hdr(URPC_REQ, NULL, sges, 2), URPC_SUCCESS);

    (void)pthread_rwlock_destroy(&g_server_channel.rw_lock);

    crypto_cipher_uninit(&cipher_opt);
}

TEST_F(crypto_test, TestCryptoEncryptDecryptRsp)
{
    // init cipher opt
    urpc_cipher_t cipher_opt = {0};
    crypto_key_t crypto_key;
    int ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // prepare data
    char hdr_sge[URPC_UT_EXT_HEADER_SIZE] = {0};
    char payload_sge[URPC_UT_USR_PAYLOAD_SIZE] = {0};
    urpc_sge_t sges[2] = {{.addr = (uint64_t)(uintptr_t)hdr_sge, .length = URPC_UT_EXT_HEADER_SIZE},
                          {.addr = (uint64_t)(uintptr_t)payload_sge, .length = URPC_UT_USR_PAYLOAD_SIZE}};

    size_t base_size = urpc_hdr_size_get(URPC_RSP, 0);
    urpc_plog_rsp_exthdr_t *exthdr = (urpc_plog_rsp_exthdr_t *)((uintptr_t)(sges[0].addr + base_size));
    urpc_plog_rsp_exthdr_fill_user_hdr_offset(exthdr, URPC_UT_HDR_ROOM_SIZE - base_size);

    // test encrypt & decrypt
    ASSERT_EQ(crypto_encrypt_with_plog_ext_hdr(URPC_RSP, &cipher_opt, sges, 2), URPC_SUCCESS);

    ASSERT_EQ(crypto_decrypt_with_plog_ext_hdr(URPC_RSP, &cipher_opt, sges, 2), URPC_SUCCESS);

    crypto_cipher_uninit(&cipher_opt);
}

TEST_F(crypto_test, TestGeneralHdrCryptoEncryptDecryptReq)
{
    // init cipher opt
    urpc_cipher_t cipher_opt = {0};
    crypto_key_t crypto_key;
    int ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // prepare data
    size_t base_size = urpc_hdr_size_get(URPC_REQ, 0);
    size_t hdr_total_size = base_size + urpc_ext_hdr_size_get(URPC_REQ, FUNC_DEF_GENERAL, 0);
    char hdr_sge[hdr_total_size] = {0};
    char payload_sge[URPC_UT_USR_PAYLOAD_SIZE] = {0};
    const char *test_payload_str = "This is a test payload string.";
    memcpy((payload_sge, test_payload_str, strlen(test_payload_str));
    urpc_sge_t sges[2] = {{.addr = (uint64_t)(uintptr_t)hdr_sge, .length = (uint32_t)hdr_total_size},
                          {.addr = (uint64_t)(uintptr_t)payload_sge, .length = URPC_UT_USR_PAYLOAD_SIZE}};

    urpc_req_head_t *base_hdr = (urpc_req_head_t *)(uintptr_t)sges[0].addr;
    urpc_req_fill_req_info_without_dma(base_hdr, 0, hdr_total_size + strlen(test_payload_str), 0, 0);
    urpc_general_req_exthdr_t *exthdr = (urpc_general_req_exthdr_t *)(base_hdr + 1);
    urpc_general_req_exthdr_fill_user_data_offset(exthdr, hdr_total_size - base_size);

    // test encrypt & decrypt
    ASSERT_EQ(crypto_encrypt_with_general_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);
    ASSERT_EQ(crypto_decrypt_with_general_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);
    // test payload unchanged
    ASSERT_STREQ(payload_sge, test_payload_str);

    // test get cipher from server channel
    (void)pthread_rwlock_init(&g_server_channel.rw_lock, NULL);
    g_server_channel.cipher_opt = &cipher_opt;
    MOCKER(server_channel_get_with_rw_lock).stubs().will(invoke(get_server_channel_mock));

    ASSERT_EQ(crypto_encrypt_with_general_ext_hdr(URPC_REQ, &cipher_opt, sges, 2), URPC_SUCCESS);
    ASSERT_EQ(crypto_decrypt_with_general_ext_hdr(URPC_REQ, NULL, sges, 2), URPC_SUCCESS);
    // test payload unchanged
    ASSERT_STREQ(payload_sge, test_payload_str);

    (void)pthread_rwlock_destroy(&g_server_channel.rw_lock);

    crypto_cipher_uninit(&cipher_opt);
}

TEST_F(crypto_test, TestGeneralHdrCryptoEncryptDecryptRsp)
{
    // init cipher opt
    urpc_cipher_t cipher_opt = {0};
    crypto_key_t crypto_key;
    int ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // prepare data
    size_t base_size = urpc_hdr_size_get(URPC_RSP, 0);
    size_t hdr_total_size = base_size + urpc_ext_hdr_size_get(URPC_RSP, FUNC_DEF_GENERAL, 0);
    char hdr_sge[hdr_total_size] = {0};
    char payload_sge[URPC_UT_USR_PAYLOAD_SIZE] = {0};
    const char *test_payload_str = "This is a test payload string.";
    memcpy((payload_sge, test_payload_str, strlen(test_payload_str));
    urpc_sge_t sges[2] = {{.addr = (uint64_t)(uintptr_t)hdr_sge, .length = (uint32_t)hdr_total_size},
                          {.addr = (uint64_t)(uintptr_t)payload_sge, .length = URPC_UT_USR_PAYLOAD_SIZE}};

    urpc_rsp_head_t *base_hdr = (urpc_rsp_head_t *)(uintptr_t)sges[0].addr;
    urpc_rsp_fill_one_req_info(base_hdr, 0, hdr_total_size + strlen(test_payload_str), NULL);
    urpc_general_rsp_exthdr_t *exthdr = (urpc_general_rsp_exthdr_t *)(base_hdr + 1);
    urpc_general_rsp_exthdr_fill_user_data_offset(exthdr, hdr_total_size - base_size);

    // test encrypt & decrypt
    ASSERT_EQ(crypto_encrypt_with_general_ext_hdr(URPC_RSP, &cipher_opt, sges, 2), URPC_SUCCESS);
    ASSERT_EQ(crypto_decrypt_with_general_ext_hdr(URPC_RSP, &cipher_opt, sges, 2), URPC_SUCCESS);
    // test payload unchanged
    ASSERT_STREQ(payload_sge, test_payload_str);

    crypto_cipher_uninit(&cipher_opt);
}

TEST_F(crypto_test, keepalive_encrypt_without_input_msg_test)
{
    int ret;
    urpc_cipher_t cipher_opt = {0};
    urpc_sge_t sge = {0};
    char keepalive_data[URPC_KEEPALIVE_HDR_SIZE] = {0};
    char backup_data[URPC_KEEPALIVE_HDR_SIZE];
    sge.addr = (uint64_t)(uintptr_t)keepalive_data;
    sge.length = URPC_KEEPALIVE_HDR_SIZE;

    for (size_t i = 0; i < URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN; i++) {
        keepalive_data[i] = i % UINT8_MAX;
    }

    crypto_key_t crypto_key;
    ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    memcpy((backup_data, keepalive_data, URPC_KEEPALIVE_HDR_SIZE);
    ret = crypto_encrypt_keepalive_req(&cipher_opt, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // keepalive header not encrypted
    ret = memcmp(backup_data, keepalive_data, URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN);
    ASSERT_EQ(ret, 0);

    ret = crypto_decrypt_keepalive_req(&cipher_opt, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    ret = memcmp(backup_data, keepalive_data, URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN);
    ASSERT_EQ(ret, 0);

    // test get cipher from server channel
    (void)pthread_rwlock_init(&g_server_channel.rw_lock, NULL);
    g_server_channel.cipher_opt = &cipher_opt;
    MOCKER(server_channel_get_with_rw_lock).stubs().will(invoke(get_server_channel_mock));

    ret = crypto_encrypt_keepalive_req(&cipher_opt, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    ret = crypto_decrypt_keepalive_req(NULL, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    (void)pthread_rwlock_destroy(&g_server_channel.rw_lock);

    crypto_cipher_uninit(&cipher_opt);
}

TEST_F(crypto_test, keepalive_encrypt_with_input_msg_test)
{
    int ret;
    urpc_cipher_t cipher_opt = {0};
    urpc_sge_t sge = {0};
    int input_msg_len = 128;
    char keepalive_data[URPC_KEEPALIVE_HDR_SIZE + input_msg_len] = {0};
    char backup_data[URPC_KEEPALIVE_HDR_SIZE + input_msg_len];
    sge.addr = (uint64_t)(uintptr_t)keepalive_data;
    sge.length = URPC_KEEPALIVE_HDR_SIZE + input_msg_len;

    for (size_t i = 0; i < URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN; i++) {
        keepalive_data[i] = i % UINT8_MAX;
    }

    (void)snprintf(keepalive_data + URPC_KEEPALIVE_HDR_SIZE, input_msg_len, "this is keepalive input msg");

    crypto_key_t crypto_key;
    ret = crypto_ssl_gen_crypto_key(&crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);
    ret = crypto_cipher_init(&cipher_opt, &crypto_key);
    ASSERT_EQ(ret, URPC_SUCCESS);

    memcpy((backup_data, URPC_KEEPALIVE_HDR_SIZE + input_msg_len);
    ret = crypto_encrypt_keepalive_req(&cipher_opt, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    // keepalive header not encrypted
    ret = memcmp(backup_data, keepalive_data, URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN);
    ASSERT_EQ(ret, 0);

    // input msg encrypted
    ret = memcmp(backup_data + URPC_KEEPALIVE_HDR_SIZE, keepalive_data + URPC_KEEPALIVE_HDR_SIZE, input_msg_len);
    ASSERT_NE(ret, 0);

    ret = crypto_decrypt_keepalive_req(&cipher_opt, &sge, 1);
    ASSERT_EQ(ret, URPC_SUCCESS);

    ret = memcmp(backup_data, keepalive_data, URPC_KEEPALIVE_HDR_SIZE - URPC_AES_CHECK_LEN);
    ASSERT_EQ(ret, 0);

    ret = memcmp(backup_data + URPC_KEEPALIVE_HDR_SIZE, keepalive_data + URPC_KEEPALIVE_HDR_SIZE, input_msg_len);
    printf("backup is [%s]\n", backup_data + URPC_KEEPALIVE_HDR_SIZE);
    printf("keepalive is [%s]\n", keepalive_data + URPC_KEEPALIVE_HDR_SIZE);
    ASSERT_EQ(ret, 0);

    crypto_cipher_uninit(&cipher_opt);
}
