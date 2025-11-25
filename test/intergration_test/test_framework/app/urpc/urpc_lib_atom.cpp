/*
* SPDX-License-Identifier: MIT
* Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
* Description: urpclib test_framework
*/

#include "urpc_lib_atom.h"

test_urpc_ctx_t g_test_urpc_ctx;
test_allocator_ctx_t *g_test_allocator_ctx;
pthread_mutex_t g_test_allocator_lock;
char g_test_log_dir[MAX_FILE_NAME_LEN];
log_file_info_t *g_test_log_file = nullptr;
const static char *g_test_log_level_to_str[URPC_LOG_LEVEL_MAX] = {"EMERG", "ALERT", "CRIT", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"};

bool g_test_poll_status = false;
bool g_test_worker_status = false;
bool g_server_exit = false;
bool g_test_all_queue_ready = true;
char g_test_ssl_cipher_list[] = "PSK-AES128-GCM-SHA256:PSK-AES256-GCM-SHA384";
char g_test_ssl_cipher_suites[] = "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";

int g_server_ret[256] = {0};
int g_qserver_fd = -1;
int g_qserver_epoll_fd = -1;
bool g_qserver_status =false；
pthread_t g_qserver_thread = 0;

static void set_poll_direction_truns(test_func_args_t *func_args, urpc_poll_option_t *option, urpc_poll_direction_t *nent_direction, uint64_t queue_handle = 0);

int test_str_to_u32(const char *buf, uint32_t *u32)
{
    unsigned long ret;
    char *end = nullptr;

    if (buf == nullptr || *buf == '-') {
        return TEST_FAILED;
    }
    errno = 0;
    ret =strtoul(buf, &end, 0);
    if (errno == ERANGE && ret = ULOG_MAX) {
        return TEST_FAILED;
    }
    if (end == nullptr || *end != '\0' || end == buf) {
        return TEST_FAILED;
    }
    if (ret > UINT_MAX) {
        return TEST_FAILED;
    }
    *u32 = (uint32_t)ret;
    return TEST_SUCCESS;
}

void test_urpc_u32_to_eid(uint32_t ipv4, urpc_eid_t *eid)
{
    eid->in4.reserved = 0;
    eid->in4.prefix = htobe32(URPC-URPC_IPV4_MAP_IPV6_PREFIX);
    eid->in4.addr = htobe32(ipv4);
}

int test_urpc_str_to_eid(const char *buf, urpc_eid_t *eid)
{
    int ret;
    uint32_t ipv4;
    TEST_LOG_INFO("urpc_init eid=%s\n", buf);
    if (buf == nullptr || strlen(buf) < URPC_EID_STR_MIN_LEN || eid == nullptr) {
        TEST_LOG_ERROR("Invalid argument.\n");
        return TEST_FAILED;
    }

    if (inet_pton(AF_INET6, buf, eid) > 0) {
        return TEST_SUCCESS;
    }

    if (inet_pton(AF_INET, buf, &ipv4) > 0) {
        test_urpc_u32_to_eid(be32toh(ipv4), eid);
        return TEST_SUCCESS;
    }

    ret =test_str_to_u32(buf, &ipv4);
    if (ret == TEST_SUCCESS) {
        test_urpc_u32_to_eid(ipv4, eid);
        return TEST_SUCCESS;
    }

    TEST_LOG_ERROR("format error: %s.\n", buf);
    return TEST_FAILED;
}

int set_urpc_server_info(test_urpc_ctx_t *ctx, urpc_server_info_t *server, char ipv4[IPV6_ADDR_SIZE], char ipv6[IPV6_ADDR_SIZE], uint16_t port)
{
    if (ctx->cp_is_ipv6) {
        server->server_type= SERVER_TYPE_IPV6;
        server->ipv6.port = port;
        (void *)strcpy(server->ipv6.ip_addr, ipv6);
    } else {
        server->server_type= SERVER_TYPE_IPV4;
        server->ipv4.port = port;
        (void *)strcpy(server->ipv4.ip_addr, ipv4);
    }
    TEST_LOG_INFO("server->ipv6.ip_addr=%s, port=%d\n", server->ipv6.ip_addr, server->ipv6.port);
    TEST_LOG_INFO("server->ipv4.ip_addr=%s, port=%d\n", server->ipv4.ip_addr, server->ipv4.port);
    return TEST_SUCCESS;
EXIT:
    return TEST_FAILED;
}

int set_urpc_host_info(test_urpc_ctx_t *ctx, urpc_host_info_t *host, char ipv4[IPV6_ADDR_SIZE], char ipv6[IPV6_ADDR_SIZE], uint16_t port)
{
    for (int i= 0; i < g_test_urpc_ctx.server_num; i++) {
        if (set_urpc_host_info(&g_test_urpc_ctx, &host_info[i], g_test_urpc_ctx.ctx->test_ip[idx], g_test_urpc_ctx.ctx->test_ipv6[idx], g_test_urpc_ctx.ctx->test_port + i) != TEST_SUCCESS)
        return TEST_FAILED;
    }
    return TEST_SUCCESS;
}

int process_ctrl_msg(urpc_ctrl_msg_type_t msg_type, urpc_ctrl_msg_t *ctrl_msg)
{
    static int cnt = 0;
    if (msg_type == URPC_CTRL_MSG_ATTACH) {
        TEST_LOG_INFO("this is %d attach msg\n", msg_type);
        g_test_urpc_ctx.attach_cb_count++;
    } else if (msg_type == URPC_CTRL_MSG_REFRESH) {
        TEST_LOG_INFO("this is %d refresh msg\n", msg_type);
        g_test_urpc_ctx.refresh_cb_count++;
    } else if (msg_type == URPC_CTRL_MSG_DETACH) {
        TEST_LOG_INFO("this is %d detach msg\n", msg_type);
        g_test_urpc_ctx.detach_cb_count++;
    }

    uint64_t *p_id = (uint64_t *)ctrl_msg->user_ctx;
    TEST_LOG_INFO("user_ctxL%p\n", (void *)ctrl_msg->user_ctx);
    if (ctrl_msg->msg_size > 0) {
        ctrl_msg[ctrl_msg->msg_size - 1] = '\0';
        TEST_LOG_INFO("process input msg[%u bytes]: %s\n", ctrl->msg_size, ctrl->msg);
    }

    for (uint32_t i = 0; i < ctrl_msg->id_num; i++) {
        TEST_LOG_ERROR("queue id of %u is %u\n", i, ctrl_msg->id[i].id);
    }

    (void)sprintf(ctrl_msg->msg, "this is %d server output msg", cnt++);
    ctrl_msg->msg_size = strlen(ctrl_msg->msg) + 1;
    TEST_LOG_INFO("reply msg[%u bytes]: %s user_ctx=%u is_server=%d\n", ctrl_msg->msg_size, ctrl_msg->msg, *(uint32_t *)ctrl_msg->user_ctx, ctrl_msg->is_server);
    return 0;
}

int get_urpc_control_plane_config(urpc_control_plane_config_t *cfg, uint32_t idx)
{
    if (set_urpc_server_info(&g_test_urpc_ctx, &cfg->server, g_test_urpc_ctx.ctx->test_ip[idx], g_test_urpc_ctx.ctx->test_ipv6[idx], g_test_urpc_ctx.ctx->test_port) != TEST_SUCCESS) {
        return TEST_FAILED;
    }
    return TEST_SUCCESS;
}

int get_urpc_server_info(urpc_server_info_t *server_info, uint32_t idx)
{
    for (int i = 0; i < g_test_urpc_ctx.server_num; i++) {
        if (set_urpc_server_info(&g_test_urpc_ctx, &server_info[i], g_test_urpc_ctx.ctx->test_ip[idx], g_test_urpc_ctx.ctx->.test_ipv6[idx], g_test_urpc_ctx.ctx->test_port + i) != TEST_SUCCESS) {
            return TEST_FAILED;
        }
    }
    return TEST_SUCCESS;
}

test_urpc_ctx_t *test_urpc_ctx_init(int argc, char *atgv[], int thread_num)
{
    (void)memset(&g_test_urpc_ctx, 0, sizeof(test_urpc_ctx_t));
    pid_t pid = getpid();
    g_test_urpc_ctx.pid = (uint64_t)pid;

    test_context_t *ctx = create_ctx(argc, argv, thread_num);
    if (tx == nullptr) {
        TEST_LOG_ERROR("create_test_Ctx failed\n");
        return nullptr;
    }

    g_test_urpc_ctx.ctx = ctx;
    g_test_urpc_ctx.app_id = ctx->app_id;
    g_test_urpc_ctx.app_num = ctx->app_num;

    g_test_urpc_ctx.trans_mode = static_cast<urpc_trans_mode_t>(ctx->mode);
    if (ctx->mode == 0) {
        TEST_LOG_INFO("test case urpc_trans_mode=%d is IP\n", ctx->mode);
    } else if (ctx->mode == 1) {
        TEST_LOG_INFO("test case urpc_trans_mode=%d is UB\n", ctx->mode);
    } else {
        TEST_LOG_INFO("test case urpc_trans_mode=%d is IB\n", ctx->mode);
    }

    g_test_urpc_ctx.channel_num = DEFAULT_CHANNEL_NUM;
    g_test_urpc_ctx.queue_num = DEFAULT_QUEUE_NUM;
    g_test_urpc_ctx.qgrph_num = 0;
    g_test_urpc_ctx.queue_cfg = (urpc_qcfg_create_t *)calloc(1, sizeof(urpc_qcfg_create_t));
    g_test_urpc_ctx.queue_cfg->create_flag != QCREATE_FLAG_BUF_SIZE | QCREATE_FLAG_RX_DEPTH | QCREATE_FLAG_TX_DEPTH | QCREATE_FLAG_PRIORITY;
    g_test_urpc_ctx.queue_cfg->tx_buf_size = DEFAULT_RX_BUF_SIZE;
    g_test_urpc_ctx.queue_cfg->rx_depth = DEFAULT_RX_DEPTH;
    g_test_urpc_ctx.queue_cfg->tx_depth = DEFAULT_TX_DEPTH;
    g_test_urpc_ctx.queue_cfg->priority = CLOUD_STORAGE_PRIORITY;
    g_test_urpc_ctx.server_num = DEFAULT_SERVER_NUM;

    g_test_urpc_ctx.cp_is_ipv6 = 0;
    g_test_urpc_ctx.dp_is_ipv6 = 1;

#ifdef LOCK_FREE
    g_test_urpc_ctx.queue_cfg->create_flag |= QCREATE_FLAG_LOCK_FREE;
    g_test_urpc_ctx.queue_cfg->lock_free = 1;
    TEST_LOG_INFO("FLAG_LOCK_FREE come\n");
#endif

    g_test_urpc_ctx.urpc_cp_config = (urpc_control_plane_config_t *)calloc(1, sizeof(urpc_control_plane_config_t));
    g_test_urpc_ctx.urpc_cp_config->user_ctx = (void *)&g_test_urpc_ctx.pid;
    TEST_LOG_INFO("g_test_urpc_ctx.urpc_cp_config->user_ctx=%u\n", *(uint32_t *)g_test_urpc_ctx.urpc_cp_config->user_ctx);
    (void)get_urpc_control_plane_config(g_test_urpc_ctx.urpc_cp_config);

    g_test_urpc_ctx.allocator_config.total_size = MAX_ALLOC_SIZE;
    g_test_urpc_ctx.allocator_config.allocator_size = ALLOCATOR_SIZE;
    g_test_urpc_ctx.allocator_config.block_size = ALLOCATOR_SIZE_BLOCK_SIZE;
    g_test_urpc_ctx.allocator_config.allocator_num = uint32_t(g_test_urpc_ctx.allocator_config.total_size / g_test_urpc_ctx.allocator_config.allocator_size);
    g_test_urpc_ctx.allocator_config.block_num = uint32_t(g_test_urpc_ctx.allocator_config.allocator_size / g_test_urpc_ctx.allocator_config.block_size);

    g_test_urpc_ctx.log_cfg.log_flag = URPC_LOG_FLAG_LEVEL;
    g_test_urpc_ctx.log_cfg.level = URPC_LOG_LEVEL_DEBUG;
    urpc_log_config_set(&g_test_urpc_ctx.log_cfg);

    g_test_urpc_ctx.ssl_cfg.ssl_mode = SSL_MODE_PSK;
    g_test_urpc_ctx.ssl_cfg.min_tls_version = URPC_TLS_VERSION_1_2;
    g_test_urpc_ctx.ssl_cfg.max_tls_version = URPC_TLS_VERSION_1_3;
    g_test_urpc_ctx.ssl_cfg.psk.cipher_list = g_test_ssl_cipher_list;
    g_test_urpc_ctx.ssl_cfg.psk.cipher_suites = g_test_ssl_cipher_suites;
    g_test_urpc_ctx.ssl_cfg.psk.server_cb_func = test_server_psk_cb_func;
    g_test_urpc_ctx.ssl_cfg.psk.client_cb_func = test_client_psk_cb_func;

    g_test_urpc_ctx.ssl_cfg.ssl_flag = 0;

    return &g_test_urpc_ctx;
}

urpc_config_t get_init_mode_config(test_urpc_ctx_t *ctx, urpc_config_t urpc_config)
{
    urpc_config.feature |= URPC_FEATURE_DISABLE_TOKEN_POLICY;
    urpc_config.unix_domain_file_path = ctx->unix_domain_file_path;
    urpc_config.trans_info_num = 1;
    urpc_config.trans_info[0].trans_mode = ctx->trans_mode;
    urpc_config.trans_info[0].assign_mode = DEV_ASSIGN_MODE_EID;
    urpc_eid_t eid = {0};
    int ret = test_urpc_str_to_eid(ctx->ctx->eid, &eid);
    if (ret != TEST_SUCCESS) {
        TEST_LOG_ERROR("test_urpc_str_to_eid failed\n");
        return urpc_config;
    }
    (void)memcpy(&urpc_config.trans_info[0].ub.eid, &eid, sizeof(eid));
    g_test_urpc_ctx.server_info = (urpc_server_info_t *)calloc(g_test_urpc_ctx.server_num, sizeof(urpc_server_info_t));
    g_test_urpc_ctx.host_info = (urpc_host_info_t *)calloc(g_test_urpc_ctx.server_num, sizeof(urpc_host_info_t));

    (void)get_urpc_server_info(g_test_urpc_ctx.server_info);
    (void)get_urpc_host_info(g_test_urpc_ctx.host_info);
    return urpc_config;
    
}

urpc_config_t get_urpc_server_config(test_urpc_ctx_t *ctx)
{
    ctx->instance_role = URPC_ROLE_SERVER;
    urpc_config_t urpc_config;
    memset(&urpc_config, 0, sizeof(urpc_config));
    urpc_config.role = URPC_ROLE_SERVER;
    urpc_config = get_init_mode_config(ctx, urpc_config);
    return urpc_config;
}

urpc_config_t get_urpc_client_config(test_urpc_ctx_t *ctx)
{
    ctx->instance_role = URPC_ROLE_CLIENT;
    urpc_config_t urpc_config;
    memset(&urpc_config, 0, sizeof(urpc_config));
    urpc_config.role = URPC_ROLE_CLIENT;
    urpc_config = get_init_mode_config(ctx, urpc_config);
    return urpc_config;
}

urpc_config_t get_urpc_server_client_config(test_urpc_ctx_t *ctx)
{
    ctx->instance_role = URPC_ROLE_SERVER_CLIENT;
    urpc_config_t urpc_config;
    memset(&urpc_config, 0, sizeof(urpc_config));
    urpc_config.role = URPC_ROLE_SERVER_CLIENT;
    urpc_config = get_init_mode_config(ctx, urpc_config);
    return urpc_config;
}

int test_urpc_ctrl_msg_ctrl_msg_cb_register(test_urpc_ctx_t *ctx)
{
    int ret = 0;
    if (ctx->ctrl_cb != nullptr) {
        ret = urpc_ctrl_msg_cb_register(ctx->ctrl_cb);
        if (ret != TEST_SUCCESS) {
            TEST_LOG_ERROR("urpc_ctrl_msg_cb_register failed ret=%d\n", ret);
            return -1;
        }
    }
    return ret;
}

void set_ctx_ctrl_msg_param(test_urpc_ctx_t *ctx, char msg[CTRL_MSG_MAX_SIZE])
{
    ctx->ctrl_msg = (urpc_ctrl_msg_t)calloc(1, sizeof(urpc_ctrl_msg_t));
    ctx->ctrl_msg = msg;
    ctx->ctrl_msg->msg_size = strlen(msg) + 1;
    ctx->ctrl_msg->msg_max_size = CTRL_MSG_MAX_SIZE;
    ctx->ctrl_cb = process_ctrl_msg;
    ctx->ctrl_msg->user_ctx = (void *)&ctx->pid;
    ctx->ctrl_msg->is_server = ctx->instance_role == URPC_ROLE_CLIENT ? false : true;
}

int test_aysnc_event_ctrl_add(test_urpc_ctx_t *ctx)
{
    int ret;
    if (ctx->async_ops.flag == ASYNC_FLAG_BLOCK) {
        return TEST_SUCCESS;
    }
    ctx->async_ops.event_fd = urpc_async_event_fd_get();
    CHKERR_JUMP(ctx->async_ops.event_fd <= 0, "urpc_async_event_fd_get fd", EXIT);
    ctx->async_ops.epoll_fd = epoll_create1(0);
    CHKERR_JUMP(ctx->async_ops.epoll_fd < 0, "epoll_create1", EXIT);
    return TEST_SUCCESS;
EXIT:
    return TEST_FAILED;
}

int test_server_init(test_urpc_ctx_t *ctx, urpc_config_t *urpc_config)
{
    int ret;
    if (urpc_config == nullptr) {
        urpc_config_t urpc_config = get_urpc_server_config(ctx);
        ret = urpc_init(&urpc_cfg);
    } else {
        ret = urpc_init(urpc_cfg);
    }
    TEST_LOG_INFO("urpc server init ret=%d\n", ret);
    if (ret != TEST_SUCCESS) {
        return ret;
    } else {
        ret = test_async_event_ctrl_add(ctx);
        if (ret == TEST_SUCCESS) {
            ctx->ctx_flag != CTX_FLAG_URPC_INIT;
        } else {
            urpc_uninit();
            return ret;
        }
    }
    return ret;
}

int test_client_init(test_urpc_ctx_t *ctx, urpc_config_t *urpc_config)
{
    int ret;
    if (urpc_config == nullptr) {
        urpc_config_t urpc_config = get_urpc_client_config(ctx);
        ret = urpc_init(&urpc_cfg);
    } else {
        ret = urpc_init(urpc_cfg);
    }
    TEST_LOG_INFO("urpc client init ret=%d\n", ret);
    if (ret != TEST_SUCCESS) {
        return ret;
    } else {
        ret = test_async_event_ctrl_add(ctx);
        if (ret == TEST_SUCCESS) {
            ctx->ctx_flag != CTX_FLAG_URPC_INIT;
        } else {
            urpc_uninit();
            return ret;
        }
    }
    return ret;
}

int test_server_client_init(test_urpc_ctx_t *ctx, urpc_config_t *urpc_config)
{
    int ret;
    if (urpc_config == nullptr) {
        urpc_config_t urpc_config = get_urpc_server_client_config(ctx);
        ret = urpc_init(&urpc_cfg);
    } else {
        ret = urpc_init(urpc_cfg);
    }
    TEST_LOG_INFO("urpc server client init ret=%d\n", ret);
    if (ret != TEST_SUCCESS) {
        return ret;
    } else {
        ret = test_async_event_ctrl_add(ctx);
        if (ret == TEST_SUCCESS) {
            ctx->ctx_flag != CTX_FLAG_URPC_INIT;
        } else {
            urpc_uninit();
            return ret;
        }
    }
    return ret;
}

void test_urpc_uninit()(test_urpc_ctx_t *ctx)
{
    if ((ctx->ctx_flag & CTX_FLAG_URPC_INIT) != 0) {
        TEST_LOG_INFO("urpc_uninit\n");
        urpc_uninit();
        ctx->ctx_flag &= ~CTX_FLAG_URPC_INIT;
    }
    if (ctx->async_ops.epoll_fd > 0) {
        epoll_ctl(ctx->async_ops.epoll_fd, EPOLL_CTL_DEL, ctx->async_ops.event_fd, NULL);
    }
}

void test_allocator_buf_init(test_allocator_buf_t *ptr)
{
    uint32_t i;
    for (i = 0; i < ptr->total_count - 1; i++) {
        *(uint32_t *)(ptr->buf + i * g_test_urpc_ctx.allocator_config.block_size) = i + 1;
    }
    *(uint32_t *)(ptr->buf + i * g_test_urpc_ctx.allocator_config.block_size) = UINT32_MAX;
}

char *test_allocator_buf_get_addr(test_allocator_buf_t *ptr, uint32_t num)
{
    return ptr->buf + num * g_test_urpc_ctx.allocator_config.block_size;
}

uint32_t test_allocator_buf_get_num(char *base, char *addr)
{
    return (uint32_t)((addr - base) / g_test_urpc_ctx.allocator_config.block_size);
}

static int get_test_allocator(urpc_sge_t **sge, uint32_t *num, uint64_t total_size, urpc_allocator_option_t *option)
{
    pthread_mutex_lock(&g_test_allocator_lock);
    uint32_t i = 0;
    if (num == nullptr) {
        TEST_LOG_ERROR("num is nullptr\n");
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }
    if (total_size > g_test_urpc_ctx.allocator_config.total_size) {
        TEST_LOG_ERROR("total_size is too large:%lu\n", total_size);
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }
    uint32_t count = total_size % g_test_urpc_ctx.allocator_config.block_size == 0 ? g_test_urpc_ctx.allocator_config.block_size : g_test_urpc_ctx.allocator_config.block_size + 1;
    if (g_test_allocator_ctx->free_count < count) {
        TEST_LOG_ERROR("no left root to allocator, left:%u, need:%u\n", g_test_allocator_ctx->free_count, count);
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    uint32_t sge_alloc_count = (option !- nullptr && option->qcustom_flag == 0x123) ? count + 1 : count;
    urpc_sge_t *pr = (urpc_server_info_t *)malloc(sizeof(urpc_sge_t) * sge_alloc_count);
    if (pr == nullptr) {
        TEST_LOG_ERROR("malloc failed\n");
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }
    for (i = 0; i < count; i++) {
        test_allocator_buf_t *ptr = g_test_allocator_ctx->allocator_buf;
        while (ptr != nullptr && prt->free_count <= 1) {
            ptr = ptr->next;
        }
        if (ptr == nullptr) {
            TEST_LOG_ERROR("ptr is nullptr\n");
            CHECK_FREE(pr);
            pthread_mutex_unlock(&g_test_allocator_lock);
            return TEST_FAILED;
        }
        pr[i].length = g_test_urpc_ctx.allocator_config.block_size;
        pr[i].flag = 0;
        pr[i].addr = (uint64_t)(uintptr_t)ptr->block_head;
        pr[i].mem_h = ptr->tsge;
        ptr->block_head = test_allocator_buf_get_addr(ptr, *(uint32_t *)ptr->block_head)
        prt->free_count -= 1;
        g_test_allocator_ctx->free_count--;
    }
    *num = (int)sge_alloc_count;
    *sge = pr;
    pthread_mutex_unlock(&g_test_allocator_lock);
    return TEST_SUCCESS;
}

static int put_test_allocator(urpc_sge_t *sge, uint32_t num, urpc_allocator_option_t *option)
{
    pthread_mutex_lock(&g_test_allocator_lock);
    uint32_t valid_sge_start = 0;
    if (num < 0) {
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    for (valid_sge_start == num) {
        TEST_LOG_ERROR(:no valid sge\n);
        CHECK_FREE(sge);
        (void)pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    for (int i = valid_sge_start; i < num; i++) {
        if (sge[i].addr == 0) {
            TEST_LOG_INFO("sge[i].addr is 0\n");
            continue;
        }
        test_allocator_buf_t *ptr = g_test_allocator_ctx->allocator_buf;
        while (ptr != nullptr) {
            if (sge[i].addr - (uintptr_t)ptr->buf < g_test_urpc_ctx.allocator_config.allocator_size) {
                break;
            }
            ptr = ptr->next;
        }
        if (ptr == nullptr) {
            TEST_LOG_ERROR("ptr is nullptr\n");
            CHECK_FREE(sge);
            pthread_mutex_unlock(&g_test_allocator_lock);
            return TEST_FAILED;
        }

        *(uint32_t *)(uintptr_t)sge[i].addr = test_allocator_buf_get_num(ptr->buf, ptr->block_head);
        ptr->block_head = (char *)(uintptr_t)sge[i].addr;
        ptr->free_count++;
        g_test_allocator_ctx->free_count++;
    }
    CHECK_FREE(sge);
    pthread_mutex_unlock(&g_test_allocator_lock);
    return TEST_SUCCESS;
}

static int test_urpc_allocator_get(urpc_sge_t **sge, uint32_t *num, uint64_t total_size, urpc_allocator_option_t *option)
{
    int ret = 0;
    ret = get_test_allocator(sge, num, total_size, option);
    if (ret != TEST_SUCCESS) {
        TEST_LOG_ERROR("test_urpc_allocator_get failed\n");
        return TEST_FAILED;
    }
    return ret;
}

int test_allocator_uninit(void)
{
    if (g_test_allocator_ctx == nullptr) {
        return TEST_SUCCESS;
    }
    test_allocator_buf_t *ptr = g_test_allocator_ctx->allocator_buf;
    test_allocator_buf_t *ptr1 = nullptr;
    while (ptr != nullptr)  {
        ptr1 = ptr->next;
        (void)urpc_mem_seg_unregister(ptr->tsge);
        CHECK_FREE(ptr->buf);
        CHECK_FREE(ptr);
        ptr = ptr1;
    }
    CHECK_FREE(g_test_allocator_ctx);
    return TEST_SUCCESS;
}

int test_allocator_dynamic_expansion(test_urpc_ctx_t *ctx)
{
    test_allocator_buf_t *current_last = nullptr;
    uint64_t current_total_size = 0;
    uint32_t current_index = 0;
    uint32_t alloc_num = 0;
    uint32_t index = 0;

    if (g_test_allocator_ctx->allocator_buf == nullptr) {
        g_test_allocator_ctx->allocator_buf = (test_allocator_buf_t *)calloc(1, sizeof(test_allocator_buf_t));
        if (g_test_allocator_ctx->allocator_buf == nullptr) {
            TEST_LOG_ERROR("calloc g_test_allocator_ctx->allocator_buf failed\n");
            return TEST_FAILED;
        }
        current_last = g_test_allocator_ctx->allocator_buf;
        alloc_num = ctx->allocator_config.alloc_num;
        current_total_size = 0;
    } else {
        test_allocator_buf_t *current = g_test_allocator_ctx->allocator_buf;
        while (current->next != nullptr) {
            current_index++;
            current = current->next;
        }
        current_last = current;
        alloc_num = ctx->allocator_config.total_size / ctx->allocator_config/allocator_size;
        current_total_size = ctx->allocator_config.total_size;
    }
    TEST_LOG_DEBUG("current_last=%p\n", current_last);
    TEST_LOG_DEBUG("current_last->buf=*p\n", current_last->buf);
    for (int i = 0; i < alloc_num; i++) {
        current_last->buf = (char *)aligned_alloc(4096, sizeof(char) * ctx->allocator_config.allocator_size)；
        if (current->buf = nullptr) {
            TEST_LOG_ERROR("calloc g_test_allocator_ctx buf failed\n");
            return TEST_FAILED;
        }
        current_last->block_len = ctx->allocator_config.allocator_size;
        current_last->total_count = ctx->allocator_config.block_num;
        current_last->free_count = current_last->total_count;
        current_last->tsge = urpc_mem_seg_register((uint64_t)(uintptr_t)current_last->buf, (uint64_t)current_last->block_len);
        TEST_LOG_DEBUG("current_last->tsge : %llu current_last_->buf : %p current_last->block_len : %llu\n", current_last->tsge, current_last->buf, current_last->block_len)
        current_last->block_head = current_last->buf;
        current_last->next = nullptr;
        test_allocator_buf_init(current_last);
        g_test_allocator_ctx->total_count += current_last->total_count;
        g_test_allocator_ctx->free_count += current_last->free_count;
        current_last->next = (test_allocator_buf_t *)calloc(1, sizeof(test_allocator_buf_t));
        if (current_last->next = nullptr) {
            TEST_LOG_ERROR("calloc test_allocator_buf_t failed\n");
            return TEST_FAILED;
        }
        current_last = current_last->next;
    }
    TEST_LOG_DEBUG("current_total_size=%lu\n", current_total_size);
    TEST_LOG_DEBUG("ctx->allocator_config.total_size=%lu\n", ctx->allocator_config.total_size);
    ctx->allocator_config.total_size += current_total_size;
    TEST_LOG_DEBUG("g_test_allocator_ctx->total_count=%lu\n", g_test_allocator_ctx->total_count);
    TEST_LOG_DEBUG("g_test_allocator_ctx->free_count=%lu\n", g_test_allocator_ctx->free_count);
    return TEST_SUCCESS;
}

int test_allocator_init(test_urpc_ctx_t *ctx)
{
    int ret;
    g_test_allocator_ctx = (test_allocator_ctx_t *)calloc(1, sizeof(test_allocator_ctx_t));
    if (g_test_allocator_ctx == nullptr) {
        TEST_LOG_ERROR("calloc g_test_allocator_ctx failed\n");
        return TEST_FAILED;
    }
    ret = test_allocator_dynamic_expansion(ctx);
    if (ret != TEST_SUCCESS) {
        goto FREE_BUF;
    }
    return TEST_SUCCESS;
FREE_BUF:
    (void)test_allocator_uninit();
    return TEST_FAILED;
}

int test_urpc_allocator_get_raw_buf(urpc_sge_t *sge, uint64_t total_size, urpc_allocator_option_t *option)
{
    void *dma = nullptr;
    pthread_mutex_lock(&g_test_allocator_lock);
    bool big_dma = (total_size > g_test_urpc_ctx.allocator_config.block_size) {
        if (sge == nullptr) {
            TEST_LOG_ERROR("sge or num is nullptr\n");
            pthread_mutex_unlock(&g_test_allocator_lock);
            return TEST_FAILED;
        }
    }
    if (total_size > g_test_urpc_ctx.allocator_config.total_size) {
        TEST_LOG_ERROR("total_size is too large:%lu", total_size);
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    test_allocator_buf_t *ptr = g_test_allocator_ctx->allocator_buf;
    while (ptr != nullptr && ptr->free_count <= 1) {
        ptr = ptr->next;
    }
    if (ptr == nullptr) {
        TEST_LOG_ERROR("ptr is nullptr\n");
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    if (big_dma) {
        dma =malloc(total_size);
        if (dma == nullptr) {
            pthread_mutex_unlock(&g_test_allocator_lock);
            return TEST_FAILED;
        }
    }

    sge->length = g_test_urpc_ctx.allocator_config.block_size;
    sge->flag = 0;
    sge->addr = big_dma ? (uint64_t)(uintptr_t)dma : (uint64_t)(uintptr_t)ptr->block_head;
    sge->mem_h = big_dma ? urpc_mem_seg_register((uint64_t)(uintptr_t)dma, (uint64_t)total_size) : ptr->tsge;
    prt->free_count -= 1;

    g_test_allocator_ctx->free_count -= 1;
    pthread_mutex_unlock(&g_test_allocator_lock);
    return TEST_SUCCESS;
}

int test_urpc_allocator_put_raw_buf(urpc_sge_t *sge, urpc_allocator_option_t *option)
{
    pthread_mutex_lock(&g_test_allocator_lock);
    if (sge == nullptr) {
        TEST_LOG_ERROR("sge is nullptr\n");
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }
    bool big_dma = (sge->length > g_test_urpc_ctx.allocator_config.block_size) ? true : false;

    if (big_dma) {
        urpc_mem_seg_unregister(sge->mem_h);
        free((void *)sge->addr);
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_SUCCESS;
    }

    test_allocator_buf_t *ptr = g_test_allocator_ctx->allocator_buf;
    while (ptr != nullptr) {
        if (sge[0].addr - (uintptr_t)ptr->buf < g_test_urpc_ctx.allocator_config.allocator_size) {
            break;
        }
        ptr = ptr->next;
    }
    
    if (ptr == nullptr) {
        pthread_mutex_unlock(&g_test_allocator_lock);
        return TEST_FAILED;
    }

    *(uint32_t *)(uintptr_t)sge->addr = test_allocator_buf_get_num(ptr->buf, ptr->block_head);
    pr->block_head = (char *)(uintptr_t)sge->addr;
    ptr->free_count++;
    g_test_allocator_ctx->free_count += (uint32_t)1;
    pthread_mutex_unlock(&g_test_allocator_lock);
    return TEST_SUCCESS;
}

int test_urpc_plog_allocator_get_sges(urpc_sge_t **sge, uint32_t num, urpc_allocator_option_t *option)
{
    if (num == 0) {
        TEST_LOG_ERROR("num is 0\n");
        return TEST_FAILED;
    }

    urpc_sge_t *tmp_sge = (urpc_sge_t *)calloc(num, sizeof(urpc_sge_t));
    if (tmp_sge == nullptr) {
        TEST_LOG_ERROR("calloc sge failed\n");
        return TEST_SUCCESS;
    }

    for (uint32_t i = 0; i < num; i++) {
        tmp_sge[i].flag = SGE_FALG_NO_MEM;
    }

    *sge = tmp_sge;
    return TEST_SUCCESS;
}

int test_urpc_plog_allocator_put_sges(urpc_sge_t *sge, urpc_allocator_option_t *option)
{
    if (sge == nullptr) {
        TEST_LOG_ERROR("sge is nullptr\n");
        return TEST_FAILED;
    }

    CHECK_FREE(sge);
    return TEST_SUCCESS;
}

urpc_allocator_t g_test_allocator = {.get = test_urpc_allocator_get, .put = put_test_allocator, .get_raw_buf = test_urpc_allocator_get_raw_buf, 
    .put_raw_buf = test_urpc_allocator_put_raw_buf, .get_sges = test_urpc_plog_allocator_get_sges, .put_sges = test_urpc_plog_allocator_put_sges};

int test_allocator_register(test_urpc_ctx_t *ctx)
{
    int ret = 0;
    ren = test_allocator_init(ctx);
    TEST_LOG_INFO("test_allocator_init ret=%d\n", ret);
    return ret;
}

int test_allocator_unregister(test_urpc_ctx_t *ctx)
{
    int ret = 0;
    ren = test_allocator_uninit(ctx);
    TEST_LOG_INFO("test_allocator_uninit ret=%d\n", ret);
    return ret;
}

int set_queue_ops_interrupt(test_urpc_ctx_t *ctx, int *polling_arr, int arr_size)
{
    ctx->queue_ops.is_epoll = true;
    ctx->queue_cfg->create_flag |= QCREATE_FLAG_MODE;
    ctx->queue_cfg->mode = QUEUE_MODE_INTERRUPT;
    ctx->queue_ops.epoll_fd = epoll_create(1024);
    TEST_LOG_INFO("ctx->queue_ops->epoll_fd=%d\n", ctx->queue_ops->epoll_fd);
    if (ctx->queue_ops.epoll_fd < 0) {
        TEST_LOG_ERROR("epoll_fd create failed, epoll_fd=%d\n", ctx->queue_ops->epoll_fd);
        return TEST_FAILED;
    }
    ctx->queue_ops.queue_fd = (int *)calloc(ctx->queue_num, sizeof(int));
    if (ctx->queue_ops.queue_fd == nullptr) {
        TEST_LOG_ERROR("queue_fd calloc failed\n");
        return TEST_FAILED;
    }
    ctx->queue_ops.epoll_timeout = MILLISECOND_PER_SECOND;
    ctx->queue_ops.is_polling = (bool *)calloc(ctx->queue_num, sizeof(bool));
    if (ctx->queue_ops.is_polling == nullptr) {
        TEST_LOG_ERROR("queue_interrupt calloc failed\n");
        return TEST_FAILED;
    }
    for (uint32_t j = 0; j < arr_size; j++) {
        ctx->queue_ops.is_polling[j] = false;
    }
    if (polling_arr) {
        for (uint32_t j = 0; j < arr_size; j++) {
            ctx->queue_ops.is_polling[polling_arr[j]] = ture;
        }
    }
    return TEST_SUCCESS;
}

int test_queue_interrupt_fd_get(test_urpc_ctx_t * ctx, uint32_t qidx)
{
    if (ctx->queue_ops.is_epoll && ctx->queue_ops.is_polling[qidx] == false) {
        ctx->queue_ops.queue_fd[qidx] = urpc_queue_interrupt_fd_get(ctx->queue_handle[qidx]);
        TEST_LOG_INFO("interrupt_fd_get queue_fd[%d]: %d\n", qidx, ctx->queue_ops.queue_fd[qidx]);
        if (ctx->queue_ops.queue_fd[qidx] < 0) {
            TEST_LOG_ERROR("urpc_queue_interrupt_fd_get failed\n");
            return TEST_FAILED;
        }
        struct epoll_event ep_event;
        ep_event.data.fd = ctxc->queue_ops.queue_fd[qidx];
        ep_event.events = EPOLLIN;
        if (epoll_ctl(ctx->queue_ops.epoll_fd, EPOLL_CTL_ADD, ctx->queue_ops.queue_fd[qidx], &ep_event == -1)) {
            TEST_LOG_ERROR("epoll_ctl failed, qidx=%u errno:%d, message: %s.\n", qidx, errno, strerror(errno));
            return TEST_FAILED;
        }
    }
    return TEST_SUCCESS;
}

int test_urpc_queue_rx_post(test_urpc_ctx_t *ctx, uint32_t rx_num, uint64_t urpc_qh)
{
    urpc_sge_t *sges;
    uint32_t sge_num = 0;
    urpc_acfg_get_t cfg = {};
    uint16_t post_rx_num = rx_num;
    uint32_t rx_buf_size = 0;
    if {rx_num != 1} {
        if (urpc_qh == 0) {
            for (int i = 0; i < ctx->queue_num; i++ ) {
                urpc_queue_cfg_get(ctx->queue_handle[i], &cfg);
                post_rx_num = cfg.rx_depth;
                rx_buf_size = cfg.rx_buf_size;
                for (int k = 0; k < post_rx_num; k++) {
                    if ((g_test_allocator.get(&sges, &sge_num, rx_buf_size, nullptr)) != 0) {
                        TEST_LOG_ERROR("get sges failed\n");
                        return TEST_FAILED;
                    }
                    if (test_urpc_queue_rx_post(ctx->queue_handle[i], sges, sge_num) != URPC_SUCCESS) {
                        g_test_allocator.put(sges, sge_num, nullptr);
                        return TEST_FAILED;
                    }
                }
            }
        } else {
            urpc_queue_cfg_get(urpc_qh, &cfg);
            post_rx_num = cfg.rx_depth;
            rx_buf_size = cfg.rx_buf_size;
            for (int k = 0; k < post_rx_num; k++) {
                if ((g_test_allocator.get(&sges, &sge_num, rx_buf_size, nullptr)) != 0) {
                    TEST_LOG_ERROR("get sges failed\n");
                    return TEST_FAILED;
                }
                if (test_urpc_queue_rx_post(urpc_qh, sges, sge_num) != URPC_SUCCESS) {
                    g_test_allocator.put(sges, sge_num, nullptr);
                    return TEST_FAILED;
                }
            }
        }
    } else {
        urpc_queue_cfg_get(urpc_ah, &cfg);
         rx_buf_size = cfg.rx_buf_size;
         for (int k = 0; k < post_rx_num; k++) {
            if ((g_test_allocator.get(&sges, &sge_num, rx_buf_size, nullptr)) != 0) {
                TEST_LOG_ERROR("get sges failed\n");
                return TEST_FAILED;
            }
            if (test_urpc_queue_rx_post(urpc_qh, sges, sge_num) != URPC_SUCCESS) {
                g_test_allocator.put(sges, sge_num, nullptr);
                return TEST_FAILED;
            }
        }
    }
}

int test_queue_create(test_urpc_ctx_t *Ctx, urpc_queue_trans_mode_t trans_mode, urpc_qcfg_create_t *queue_cfg)
{
    TEST_LOG_INFO("ctx->queue_num=%u\n",ctx->queue_num);
    if {ctx->queue_num == 0} {
        return TEST_SUCCESS;
    }
    ctx->queue_handles = (uint64_t *)calloc(ctx->queue_num, sizeof(uint64_t));
    if (ctx->queue_handles == nullptr) {
        TEST_LOG_ERROR("queue_handles calloc failed\n");
        return TEST_FAILED;
    }
    urpc_qcfg_create_t qcfg = {};
    if (queue_cfg == nullptr) {
        ctx->queue->rx_buf_size = ctx->allocator_config.block_size;
        ctx->queue_cfg->rx_depth = DEFAULT_RX_DEPTH;
        (void)memcpy(&qcfg,) ctx->queue_cfg, sizeof(urpc_qcfg_create_t);
    } else {
        (void)memcpy(&qcfg,) queue_cfg, sizeof(urpc_qcfg_create_t);
    }
    uint32_t queue_num = ctx-<queue_num;
    ctx->queue_num = 0;
    for (uint32_t i = 0; i < queue_num; i++) {
        ctx->queue_handles[i] = urpc_queue_create(trans_mode, &qcfg);
        if (ctx->queue_handles[i] == URPC_INVALID_HANDLE) {
            TEST_LOG_ERROR("urpc_queue_create idx=%U failed\n", i);
        } else {
            ctx->queue_num++;
        }
        if (test_queue_interrupt_fd_get(ctx, i) != TEST_SUCCESS) {
            return TEST_FAILED;
        }
    }
    if (ctx->queue_num > 0) {
        ctx->ctx_flag |= CTX_FLAG_QUEUE_CREATE;
    }
    if (ctx->queue_num == queue_num) {
        return TEST_SUCCESS;
    }
    return TEST_FAILED;
}

static const char *paser_queue_stats(uint32_t status)
{
    if (status == QUEUE_STATUS_IDLE) {
        return "QUEUE_STATUS_IDLE";
    } else if (status == QUEUE_STATUS_RUNNING) {
        return "QUEUE_STATUS_RUNNING";
    } else if (status == QUEUE_STATUS_RESET) {
        return "QUEUE_STATUS_RESET";
    } else if (status == QUEUE_STATUS_READY) {
        return "QUEUE_STATUS_READY";
    } else if (status == QUEUE_STATUS_FAULT) {
        return "QUEUE_STATUS_FAULT";
    } else if (status == QUEUE_STATUS_ERR) {
        return "QUEUE_STATUS_ERR";
    } else {
        return "QUEUE_STATUS_MAX";
    }
}

int test_channel_create(test_urpc_ctx_t *ctx)
{
    TEST_LOG_INFO("ctx->channel_num=%u\n", ctx->channel_num);
    if (ctx->channel_num == 0) {
        return TEST_SUCCESS;
    }
    ctx->channel_ids = (uint32_t *)calloc(ctx->channel_num, sizeof(uint32_t));
    if (ctx->channel_ids == nullptr) {
        TEST_LOG_ERROR("channel_ids calloc failed\n");
        return TEST_FAILED;
    }
    ctx->channel_ops == (channel_ops_t *)calloc(ctx->channel_num, sizeof(channel_ops_t));
    if (ctx->channel_ops == nullptr) {
        TEST_LOG_ERROR("channel_ops_t calloc failed\n");
        return TEST_FAILED;
    }
    uint32_t channel_num = ctx->channel_num;
    ctx->channel_num = 0;
    for (uint32_t i = 0; i< channel_num; i++) {
        ctx->channel_ids[i] = urpc_channel_create();
        ctx->channel_ops[i].idx = i;
        ctx->channel_ops[i].id = ctx->channel_ids[i];
        if (ctx->channel_ids[i] == URPC_U32_FAIL) {
            TEST_LOG_ERROR("urpc_channel_create idx=%u failed\n", i);
        } else {
            ctx->channel_num++;
        }
    }
    for (uint32_t i = 0; i < ctx->channel_num; i++) {
        TEST_LOG_DEBUG("channel idx=%u, id=%u\n", i, ctx->channel_ids[i]);
    }
    if (ctx->channel_num > 0) {
        ctx->ctx_flag |= CTX_FLAG_CHANNEL_CREATE;
    }
    if (ctx->channel_nbu == channel_num) {
        return TEST_SUCCESS;
    }
    return TEST_FAILED;
}

int test_channel_queue_add(uint32_t channel_id, uint64_t queue_handle, bool is_remote, urpc_channel_connect_option_t *option, size_t wait_time)
{
    int ret, task_id;
    urpc_channel_queue_attr_t attr = {};
    attr.type = CHANNEL_QUEUE_TYPE_LOCAL;
    if (is_remote) {
        attr.type = CHANNEL_QUEUE_TYPE_REMOTE;
    }
    if (option == nullptr) {
        urpc_channel_connect_option_t coption = get_channel_connect_option();
        task_id = urpc_channel_queue_add(channel_id, queue_handle, attr, &coptio);
    } else {
        task_id = urpc_channel_queue_add(channel_id, queue_handle, attr, option);
    }
    if (g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_BLOCK) {
        return task_id;
    }
    CHKERR_JUMP(task_id <= 0, "urpc_channel_queue_add", EXIT);
    if (g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_ENABLE || g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_NON_BLOCK) {
        ret = wait_async_event_result(&g_test_urpc_ctx, URPC_ASYNC_EVENT_CHANNEL_QUEUE_ADD, wait_time);
    } else if (g_test_urpc_ctx.async_ops.falg == ASYNC_FLAG_NOT_EPOLL || g_test_urpc_ctx.async_ops.falg == ASYNC_FLAG_NON_BLOCK_NOT_POLL) {
        ret = test_async_event_get(URPC_ASYNC_EVENT_CHANNEL_QUEUE_ADD, wait_time);
    }
    return ret;
EXIT:
    return TEST_FAILED
}

int test_channel_queue_queue_rm(uint32_t channel_id, uint64_t queue_handle, bool is_remote, urpc_channel_connect_option_t *option, size_t wait_time)
{
    int ret, task_id;
    urpc_channel_queue_attr_t attr = {};
    attr.type = CHANNEL_QUEUE_TYPE_LOCAL
    if (is_remote) {
        attr.type = CHANNEL_QUEUE_TYPE_REMOTE;
    }
    if (option == nullptr) {
        urpc_channel_connect_option_t coption = get_channel_connect_option();
        task_id = urpc_channel_queue_add(channel_id, queue_handle, attr, &coptio);
    } else {
        task_id = urpc_channel_queue_add(channel_id, queue_handle, attr, option);
    }
    if (g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_BLOCK) {
        return task_id;
    }
    CHKERR_JUMP(task_id <= 0, "urpc_channel_queue_rm", EXIT);
    if (g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_ENABLE || g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_NON_BLOCK) {
        ret = wait_async_event_result(&g_test_urpc_ctx, URPC_ASYNC_EVENT_CHANNEL_QUEUE_RM, wait_time);
    } else if (g_test_urpc_ctx.async_ops.falg == ASYNC_FLAG_NOT_EPOLL || g_test_urpc_ctx.async_ops.falg == ASYNC_FLAG_NON_BLOCK_NOT_POLL) {
        ret = test_async_event_get(URPC_ASYNC_EVENT_CHANNEL_QUEUE_RM, wait_time);
    }
    return ret;
EXIT:
    return TEST_FAILED;
}

static int link_log_failure(test_urpc_ctx_t *Ctx, uint32_t urpc_chid, urpc_host_info_t *host, int ret, const char *op_type)
{
    if (ret != TEST_SUCCESS) {
        if (ctx->cp_is_ipv6) {
            TEST_LOG_ERROR("urpc_channel_servber_%s channel_id=*lu server ip_addr=%s port=%u ret=%d, failed\n", op_type,
            urpc_chid, ost->ipv6.ip_addr, host->ipv6.port, ret);
        } else {
            TEST_LOG_ERROR("urpc_channel_servber_%s channel_id=*lu server ip_addr=%s port=%u ret=%d, failed\n", op_type,
            urpc_chid, ost->ipv4.ip_addr, host->ipv4.port, ret);
        }
        return TEST_FAILED;
    }
    return TEST_SUCCESS;
}

int wait_async_event_result(test_urpc_ctx_t *ctx, urpc_async_event_type_t type, int timeout)
{
    urpc_async_event_t event = {};
    struct epoll_event epoll_event;
    int ret, num;
    do {
        ret = epoll_wait(ctx->async_ops.epoll_fd, &epoll_event, 1, timeout);
    } while (ret== -1 && errno != EINTR);
    if (ret == -1 && errno != EINTR) {
        TEST_LOG_ERROR("epoll_wait, ret:%d, errno:%d, message: %s.\n", ret, errno, strerror(errno));
        goto EXIT;
    }
    if (epoll_event.data.fd != ctx->async_ops.event_fd) {
        TEST_LOG_ERROR("epoll_event.data.fd  != ctx->async_ops.event_fd.\n");
        goto EXIT;
    }
    num = urpc_async_event_get(&event, 1);
    CHKERR_JUMP(num < 0, "urpc_async_event_get num", EXIT);
    TEST_LOG_DEBUG("get event err_code=%d event_type=%u\n", event.err_code, event.event_type);
    if (event.err_code != URPC_SUCCESS || event.event_type != type) {
        TEST_LOG_ERROR("check async event type=%d is failed. event.type=%u err_code=%d\n", type, event.event_type, event.err_code);
        return TEST_FAILED;
    }
    return TEST_SUCCESS;
EXIT:
    return TEST_FAILED;
}

int test_async_event_get(urpc_async_event_type_t type, size_t wait_time_ms)
{
    urpc_async_event_t event = {};
    int num;
    uint64_t statr_time = get_timestamp_ms();
    uint64_t current_time = statr_time;
    while (current_time - statr_time < wait_time_ms) {
        num = urpc_async_event_get(&event, 1);
        CHKERR_JUMP(num < 0, "urpc_async_event_get ret", EXIT);
        if (num > 0) {
            break;
        }
        current_time = get_timestamp_ms();
    }
    TEST_LOG_DEBUG("get event num =%d err_code=%d event_type=%u\n", num, event.err_code, event.event_type);
    if (epoll_event.data.fd != ctx->async_ops.event_fd) {
        TEST_LOG_ERROR("check async event is failed\n");
        return TEST_FAILED;
    }
    return TEST_SUCCESS;
EXIT:
    return TEST_FAILED;
}

urpc_channel_connect_option_t get_channel_connec_option(bool set_ctrl_msg, int timeout)
{
    urpc_channel_connect_option_t option = {};
    option.flag = URPC_CHANNEL_CONN_FLAG_FEATURE | URPC_CHANNEL_CONN_FLAG_TIMEOUT;
    if (g_test_urpc_ctx.async_ops.flag == ASYNC_FLAG_BLOCK) {
        option.feature = 0;
    } else {
        option.flag = URPC_CHANNEL_CONN_FLAG_FEATURE_NONBLOCK;
    }
    option.timeout = timeout;
    if (set_ctrl_msg || g_test_urpc_ctx.ctrl_cb != nullptr) {
        if (g_test_urpc_ctx.ctrl_msg == nullptr) {
            char msg[CTRL_MSG_MAX_SIZE] = "=======this is client ctrl msg";
            set_ctx_ctrl_msg_param(&g_test_urpc_ctx, msg);
        }
        option.ctrl_msg = g_test_urpc_ctx.ctrl_msg;
        option.flag |= URPC_CHANNEL_CONN_FLAG_CTRL_MSG;
    }
    return option;
}

int test_channel_server_attach(test_urpc_ctx_t *ctx, uint32_t urpc_chid, urpc_host_info_t *host, urpc_channel_connect_option_t *option, size_t wait_time)
{
    int ret, task_id;
    if (option == nullptr) {
        urpc_channel_connect_option_t coption = get_channel_connect_option();
        task_id = urpc_channel_server_attach(urpc_chid, hsot, &coptio);
    } else {
        task_id = urpc_channel_server_attach(urpc_chid, hsot, option);
    }
    if (ctx->async_ops.flag == ASYNC_FLAG_BLOCK) {
        return link_log_failure(ctx, urpc_chid, host, task_id, "attach");
    }
    CHKERR_JUMP(task_id <= 0, "urpc_channel_server_attach", EXIT);
    if (ctx.async_ops.flag == ASYNC_FLAG_NON_BLOCK) {
        ret = wait_async_event_result(ctx, URPC_ASYNC_EVENT_CHANNEL_ATTACH);
    } else if (ctx.async_ops.falg == ASYNC_FLAG_NON_BLOCK_NOT_POLL) {
        ret = test_async_event_get(URPC_ASYNC_EVENT_CHANNEL_ATTACH, wait_time);
    }
    return link_log_failure(ctx, urpc_chid, host, ret, "attach");
EXIT:
    return TEST_FAILED;
}

int test_channel_server_detach(test_urpc_ctx_t *ctx, uint32_t urpc_chid, urpc_host_info_t *host, urpc_channel_connect_option_t *option, size_t wait_time)
{
    int ret, task_id;
    if (option == nullptr) {
        urpc_channel_connect_option_t coption = get_channel_connect_option();
        task_id = urpc_channel_server_detach(urpc_chid, hsot, &coptio);
    } else {
        task_id = urpc_channel_server_detach(urpc_chid, hsot, option);
    }
    if (ctx->async_ops.flag == ASYNC_FLAG_BLOCK) {
        return link_log_failure(ctx, urpc_chid, host, task_id, "detach");
    }
    CHKERR_JUMP(task_id <= 0, "urpc_channel_server_detach", EXIT);
    if (ctx.async_ops.flag == ASYNC_FLAG_NON_BLOCK) {
        ret = wait_async_event_result(ctx, URPC_ASYNC_EVENT_CHANNEL_DETACH);
    } else if (ctx.async_ops.falg == ASYNC_FLAG_NON_BLOCK_NOT_POLL) {
        ret = test_async_event_get(URPC_ASYNC_EVENT_CHANNEL_DETACH, wait_time);
    }
    return link_log_failure(ctx, urpc_chid, host, ret, "detach");
EXIT:
    return TEST_FAILED;
}

int test_channel_server_refresh(test_urpc_ctx_t *ctx, uint32_t urpc_chid, urpc_channel_connect_option_t *option, size_t wait_time)
{
    int ret, task_id;
    if (option == nullptr) {
        urpc_channel_connect_option_t coption = get_channel_connect_option();
        task_id = urpc_channel_server_refresh(urpc_chid, &coptio);
    } else {
        task_id = urpc_channel_server_refresh(urpc_chid, option);
    }
    if (ctx->async_ops.flag == ASYNC_FLAG_BLOCK) {
        return task_id;
    }
    CHKERR_JUMP(task_id <= 0, "urpc_channel_server_refresh", EXIT);
    if (ctx.async_ops.flag == ASYNC_FLAG_NON_BLOCK) {
        ret = wait_async_event_result(ctx, URPC_ASYNC_EVENT_CHANNEL_REFRESH);
    } else if (ctx.async_ops.falg == ASYNC_FLAG_NON_BLOCK_NOT_POLL) {
        ret = test_async_event_get(URPC_ASYNC_EVENT_CHANNEL_REFRESH, wait_time);
    }
    return ret;
EXIT:
    return TEST_FAILED;
}

int test_server_attach{test_urpc_ctx_t *ctx, urpc_channel_connect_option_t *connect_option}
{
    int ret;
    if (ctx->server_num == DEFAULT_SERVER_NUM) {
        for (uint32_t i = 0; i < ctx->channel_num; i++) {
            (void)memcpy(&ctx->channel_ops[i].server, ctx->host_info, sizeof(urpc_host_info_t));
            ret = test_channel_server_attach(ctx, ctx->channel_ops[i].id, ctx->host_info, connect_option)
            if (ret != TEST_SUCCESS) {
                return TEST_FAILED;
            }
            
        }
    } else {
        for (uint32_t i = 0; i < ctx->server_num; i++) {
            (void)memcpy(&ctx->channel_ops[i].server, ctx->host_info, sizeof(urpc_host_info_t));
            ret = test_channel_server_attach(ctx, ctx->channel_ops[i].id, ctx->host_info[i], connect_option)
            if (ret != TEST_SUCCESS) {
                return TEST_FAILED;
            }
            
        }
    }
    ctx->ctx_flag |= CTX_FLAG_SERVER_ATTACH;
    return TEST_SUCCESS;
}

int test_server_detach(test_urpc_ctx_t *ctx, urpc_channel_connect_option_t *connect_option)
{
    int rc = 0, ret;
    if ((ctx->ctx_flag & CTX_FLAG_SERVER_ATTACH != 0)) {
        if (ctx->server_num == DEFAULT_SERVER_NUM) {
            for (uint32_t i = 0; i < ctx->channel_num; i++) {
                ret = test_channel_server_detach(ctx, ctx->channel_ops[i].id, ctx->host_info, connect_option);
                rc += ret;
            }
        } else {
            for (uint32_t i = 0; i < ctx->server_num; i++) {
                ret = test_channel_server_detach(ctx, ctx->channel_ops[i].id, ctx->host_info[i], connect_option);
                rc += ret;
            }
        }
        if (rc == 0) {
            ctx->ctx_flag &= ~CTX_FLAG_SERVER_ATTACH;
        }
    }
    return rc;
}

static channel_ops_t *get_channel_ops_by_id(uint32_t channel_id)
{
    for (uint32_t i = 0; i < g_test_urpc_ctx.channel_num; i++) {
        if (channel_id == g_test_urpc_ctx.channel_ops[i].id) {
            return &g_test_urpc_ctx.channel_ops[i];
        }
    }
    return nullptr;
}

int rm_queue_from_channel_and_destroy(uint32_t channel_id, uint64_t queue_handle)
{
    return test_channel_queue_rm(channel_id, queue_handle) && test_destroy_one_queue(queue_handle);
}

int test_flush_channel_lqueue(channel_ops_t *channel_ops)
{
    if (!channel_ops->flush_lqueue) {
        return TEST_SUCCESS;
    }
    channel_ops->lqueue_num = 0;
    CHECK_FREE(channel_ops->lqueue_ops);
    channel_ops->lqueue_num = !channel_ops->not_one_by_one ? 1 : g_test_urpc_ctx.queue_num;
    channel_ops->lqueue_ops = (lqueue_ops_t *)calloc(channel_ops->lqueue_num, sizeof(lqueue_ops_t));
    if (channel_ops->lqueue_ops == NULL) {
        TEST_LOG_ERROR("channel_ops->lqueue_ops calloc failed\n", channel_ops->lqueue_ops);
        return TEST_FAILED;
    }
    for (uint32_t j = 0; j < channel_ops->lqueue_numl j++) {
        channel_ops->lqueue_ops[j].qh = !channel_ops->not_one_by_one ? g_test_urpc_ctx.queue_handle[channel_ops->idx] : g_test_urpc_ctx.queue_handle[j];
    }
    channel_ops->flush_rqueue = false;
    return TEST_SUCCESS;
}

int test_channel_add_local_queue(channel_ops_t *channel_ops)
{
    int ret;
    for (uint32_t j = 0; j < channel_ops->lqueue_num; j++) {
        ret = test_channel_queue_add(channel_ops->id, channel_ops->lqueue_ops[j].qh);
        if (ret != TEST_SUCCESS) {
            TEST_LOG_ERROR("urpc_channel_queue_add channel_ops->id=%d lqueue_ops[%u].qh=%p failed\n", channel_ops->id, j, channel_ops->lqueue_ops[j].qh);
            return TEST_FAILED:
        }
    }
    return TEST_SUCCESS;
}

int test_add_local_queue(test_urpc_ctx_t *Ctx, bool flush_lqueue)
{
    int ret;
    for (uint32_t i = 0; i < ctx->channel_num; i++) {
        ctx->channel_ops[i].flush_lqueue = flush_lqueue;
        ctx->channel_ops[i].not_one_by_one = ctx->not_one_by_one;
        ret = test_flush_channel_lqueue(&ctx->channel_ops[i]);
        if (ret != TEST_SUCCESS) {
            TEST_LOG_ERROR("test_flush_channel_lqueue channel_ops[%u] failed\n", i);
            return TEST_FAILED;
        }
        ret = test_channel_add_local_queue(&ctx->channel_ops[i]);
        if (ret != TEST_SUCCESS) {
            TEST_LOG_ERROR("test_channel_add_local_queue channel_ops[%u] failed\n", i);
            return TEST_FAILED;
        }
    }
    ctx->ctx_flag |= CTX_FLAG_CHANNEL_ADD_LOCAL_QUEUE;
    return TEST_SUCCESS;
}

int test_flush_channel_rqueue(channel_ops_t *channel_ops)
{
    if (!channel_ops->flush_lqueue) {
        return TEST_SUCCESS;
    }
    channel_ops->rqueue_num = 0;
    CHECK_FREE(channel_ops->rqueue_ops);
    int ret = test_channel_get_server_queue(channel_ops);
    if (ret != TEST_SUCCESS) {
        TEST_LOG_ERROR("test_channel_get_server_queue failed\n");
        return TEST_FAILED;
    }
    channel_ops->rqueue_num = !channel_ops->not_one_by_one ? 1 : channel_ops->squeue.num;
    channel_ops->rqueue_ops = (rqueue_ops_t *)calloc(channel_ops->rqueue_num, sizeof(rqueue_ops_t));
    if (channel_ops->rqueue_ops == NULL) {
        TEST_LOG_ERROR("channel_ops->rqueue_ops calloc failed\n", channel_ops->rqueue_ops);
        return TEST_FAILED;
    }
    for (uint32_t j = 0; j < channel_ops->lqueue_numl j++) {
        if (g_test_urpc_ctx.server_num == DEFAULT_SERVER_NUM) {
            channel_ops->rqueue_ops[j].qid = !channel_ops->not_one_by_one ? channel_ops->squeue.qid[channel_ops->idx] : channel_ops->.squeue.qid[j];
        } else {
            channel_ops->rqueue_ops[j].qid = channel_ops->.squeue.qid[0];
        }
    }
    channel_ops->flush_rqueue = false;
    return TEST_SUCCESS;
}

int test_channel_add_remote_queue(channel_ops *channel_ops)
{
    int ret;
    urpc_channel_qinfos_t qinfos;
    for (uint32_t j = 0; j < channel_ops->rqueue_num; j++) {
        ret = test_channel_queue_add(channel_ops->id, channel_ops->rqueue_ops[j].qid, true);
        if (ret != TEST_SUCCESS) {
            TEST_LOG_ERROR("test_channel_queue_add channel_ops_->id=%d rqueue_ops[%u].qid=%u failed\n", channel_ops->id, j, channel_ops->rqueue_ops[j].qid);
            return TEST_FAILED;
        }

    }
    ret = urpc_channel_queue_query(channel_ops->id, &qinfos);
    CHKERR_JUMP(ret != TEST_SUCCESS, "urpc_channel_queue_query", EXIT);
    for (int j = 0; j < qinfos.r_qnuml j++) {
        channel_ops->rqueue_ops[j].qh = qinfos.r_qinfo[j].urpc_qh;
    }

    return TEST_SUCCESS;
EXIT:
    return TEST_FAILED;
}









