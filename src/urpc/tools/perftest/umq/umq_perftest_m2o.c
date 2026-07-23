/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: UMQ multi-to-one perftest with shared JFR.
 *              Supports two modes:
 *                * Sub UMQ mode (default): N Sub UMQs share Main's jfr via SHARE_RQ
 *                * Logic UMQ mode (--logic-umq): N Logic UMQs share Main's jfr and borrow
 *                  jetty pairs from the global jetty pool; each Logic UMQ has a hidden
 *                  donor Sub UMQ keeping its jetty node alive in the pool
 * Create: 2026-5-26
 */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#include "ub_get_clock.h"
#include "umq_api.h"
#include "umq_pro_api.h"
#include "umq_dfx_api.h"
#include "umq_errno.h"

#define DEFAULT_BUF_SIZE            4096
#define DEFAULT_RX_DEPTH            512
#define DEFAULT_TX_DEPTH            512
#define DEFAULT_SERVER_RX_DEPTH     4096
#define DEFAULT_TEST_ROUND          100000
#define DEFAULT_TCP_PORT            19878
#define DEFAULT_CLIENT_NUM          1
#define MAX_CLIENT_NUM              128
#define MAX_POLL_BATCH              64
#define SPSC_QUEUE_SIZE             4096
#define UMQ_MAX_BIND_INFO_SIZE      8196
#define SOCKET_TIMEOUT_SEC          60
#define INTERRUPT_WAIT_MS           3000
#define LATENCY_TAIL                2
#define INLINE_LIMIT_SIZE           32
#define INLINE_ENABLE               1
#define PERFTEST_1M                 (1000000)
#define PERFTEST_1MB                (0x100000)
#define PERCENT_NUM                 100
#define SYNC_MSG_LEN                4
#define WAIT_UMQ_READY_ROUND        10000
#define WAIT_UMQ_READY_US           100

#define DEFAULT_PIPELINE_DEPTH      1
#define MAX_PIPELINE_DEPTH          512

typedef struct m2o_config {
    char dev_name[128];
    uint16_t eid_idx;
    uint32_t buf_size;
    uint32_t rx_depth;
    uint32_t tx_depth;
    uint32_t test_round;
    uint16_t tcp_port;
    char server_ip[46];
    bool is_server;
    uint32_t client_num;
    uint32_t pipeline_depth;
    uint32_t trans_mode;
    umq_tp_type_t tp_type;
    umq_buf_mode_t buf_mode;
    bool enable_flow_control;
    bool use_atomic_window;
    uint16_t initial_credit;
    uint16_t max_credits_request;
    uint32_t fc_timeout_ms;
    bool use_logic_umq;
    uint32_t donor_sub_num;
    uint32_t tp_handle_cnt;
} m2o_config_t;

typedef struct m2o_umq_ctx {
    uint64_t sub_umqh;
    uint32_t client_idx;
} m2o_umq_ctx_t;

typedef struct server_ctx server_ctx_t;

typedef struct m2o_sub_umq_info {
    uint64_t umqh;
    m2o_umq_ctx_t *umq_ctx;
    int client_fd;
    bool valid;
    pthread_t worker_thread;
    volatile bool worker_stop;
    server_ctx_t *srv_ctx;

    umq_buf_t **dispatch_bufs;
    atomic_uint dispatch_head;
    atomic_uint dispatch_tail;
    sem_t dispatch_sem;

    umq_buf_t **pending_bufs;
    uint32_t pending_head;
    uint32_t pending_tail;
    uint32_t pending_count;
    uint32_t pending_capacity;

    umq_buf_t **tx_free_bufs;
    uint32_t tx_free_count;
    uint32_t tx_free_capacity;

    uint32_t tp_handle_cnt;
} m2o_sub_umq_info_t;

struct server_ctx {
    m2o_config_t *cfg;
    uint64_t main_umqh;
    m2o_sub_umq_info_t sub_umqs[MAX_CLIENT_NUM];
    uint64_t donor_umqhs[MAX_CLIENT_NUM];
    uint32_t client_cnt;
    int epoll_fd;
    int main_rx_fd;
    volatile bool force_quit;
    atomic_uint_fast64_t total_recv_cnt;
    atomic_uint_fast64_t total_send_cnt;

    // Dedicated TX poller thread for each donor Sub UMQ (Logic mode only)
    pthread_t sub_tx_poller[MAX_CLIENT_NUM];
    volatile bool poller_stop;
    uint32_t tp_handle_idx[1024];
    uint32_t tp_handle_cnt;
};

typedef struct client_ctx {
    m2o_config_t *cfg;
    uint64_t main_umqh;
    uint64_t sub_umqh;
    uint64_t donor_umqh;
    int tcp_fd;
    uint64_t *cycles;
    uint32_t iter_count;
    volatile bool force_quit;
    uint32_t tp_handle_idx[1024];
    uint32_t tp_handle_cnt;
} client_ctx_t;

static volatile bool g_force_quit = false;

static void signal_handler(int signum)
{
    (void)signum;
    g_force_quit = true;
}

static void init_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void usage(void)
{
    printf("UMQ Multi-to-One Perftest with Shared JFR\n");
    printf("Usage:\n");
    printf("  --server              Run as server\n");
    printf("  --client              Run as client\n");
    printf("  -d, --dev <dev>       Device name\n");
    printf("  -i, --server-ip <ip>  Server IP address\n");
    printf("  -p, --port <port>     TCP port (default: %d)\n", DEFAULT_TCP_PORT);
    printf("  -s, --size <size>     Message size (default: %d)\n", DEFAULT_BUF_SIZE);
    printf("  -n, --num <num>       Number of clients (server) or test iterations (client, default: %d)\n",
           DEFAULT_TEST_ROUND);
    printf("  --rx-depth <depth>    RX depth (default: %d)\n", DEFAULT_RX_DEPTH);
    printf("  --tx-depth <depth>    TX depth (default: %d)\n", DEFAULT_TX_DEPTH);
    printf("  --pipeline <depth>    Pipeline depth, in-flight packets (default: %d)\n", DEFAULT_PIPELINE_DEPTH);
    printf("  --eid-index <idx>     EID index (default: 0)\n");
    printf("  --trans-mode <mode>   Transport mode (default: 0=UB)\n");
    printf("  --tp-type <type>      TP type (default: 1=CTP)\n");
    printf("  --flow-control        Enable flow control (default: on)\n");
    printf("  --no-flow-control     Disable flow control\n");
    printf("  --use-atomic-window   Use atomic window for flow control (default: on)\n");
    printf("  --initial-credit <n>  Initial credit for flow control (default: 4)\n");
    printf("  --max-credit-req <n>  Max credits per request (default: 4)\n");
    printf("  --fc-timeout <ms>     Flow control timeout in ms (default: 0)\n");
    printf("  --logic-umq           Use Logic UMQ (shared jfr + jetty pool) instead of Sub UMQ (default: off)\n");
    printf("                          Forces Main UMQ tp_mode=RM and creates hidden donor Sub UMQ per client\n");
    printf("  --donor-num <n>       Number of hidden donor Sub UMQs in Logic mode (default: client_num)\n");
    printf("  -h, --help            Show this help\n");
}

static int parse_arguments(int argc, char **argv, m2o_config_t *cfg)
{
    if (argc == 1) {
        usage();
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->buf_size = DEFAULT_BUF_SIZE;
    cfg->rx_depth = DEFAULT_RX_DEPTH;
    cfg->tx_depth = DEFAULT_TX_DEPTH;
    cfg->test_round = DEFAULT_TEST_ROUND;
    cfg->tcp_port = DEFAULT_TCP_PORT;
    cfg->client_num = DEFAULT_CLIENT_NUM;
    cfg->pipeline_depth = DEFAULT_PIPELINE_DEPTH;
    cfg->eid_idx = 0;
    cfg->trans_mode = UMQ_TRANS_MODE_UB;
    cfg->tp_type = UMQ_TP_TYPE_CTP;
    cfg->buf_mode = UMQ_BUF_SPLIT;
    cfg->enable_flow_control = true;
    cfg->use_atomic_window = true;
    cfg->initial_credit = 128;
    cfg->max_credits_request = 128;
    cfg->fc_timeout_ms = 0;
    cfg->use_logic_umq = false;
    cfg->donor_sub_num = 0;
    cfg->tp_handle_cnt = 1;

    static struct option long_options[] = {
        {"server",     no_argument,       NULL, 'S'},
        {"client",     no_argument,       NULL, 'C'},
        {"dev",        required_argument, NULL, 'd'},
        {"server-ip",  required_argument, NULL, 'i'},
        {"port",       required_argument, NULL, 'p'},
        {"size",       required_argument, NULL, 's'},
        {"num",        required_argument, NULL, 'n'},
        {"rx-depth",   required_argument, NULL, 'R'},
        {"tx-depth",   required_argument, NULL, 'T'},
        {"pipeline",   required_argument, NULL, 'D'},
        {"eid-index",  required_argument, NULL, 'E'},
        {"trans-mode", required_argument, NULL, 'M'},
        {"tp-type",    required_argument, NULL, 'P'},
        {"flow-control",    no_argument,       NULL, 'F'},
        {"no-flow-control", no_argument,       NULL, 'G'},
        {"use-atomic-window", no_argument,     NULL, 'A'},
        {"initial-credit",  required_argument, NULL, 'I'},
        {"max-credit-req",  required_argument, NULL, 'L'},
        {"fc-timeout",      required_argument, NULL, 'O'},
        {"logic-umq",  no_argument,       NULL, 'l'},
        {"tp_handle_cnt",  required_argument, NULL, 't'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while (1) {
        int c = getopt_long(argc, argv, "d:i:p:s:n:hSCFGAI:L:O:D:lX:", long_options, NULL);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'S':
                cfg->is_server = true;
                break;
            case 'C':
                cfg->is_server = false;
                break;
            case 'd':
                memcpy(cfg->dev_name, optarg, strlen(optarg));
                break;
            case 'i':
                memcpy(cfg->server_ip, optarg, strlen(optarg));
                break;
            case 'p':
                cfg->tcp_port = (uint16_t)strtoul(optarg, NULL, 0);
                break;
            case 's':
                cfg->buf_size = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'n':
                cfg->client_num = (uint32_t)strtoul(optarg, NULL, 0);
                cfg->test_round = cfg->client_num;
                break;
            case 'R':
                cfg->rx_depth = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'T':
                cfg->tx_depth = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'D':
                cfg->pipeline_depth = (uint32_t)strtoul(optarg, NULL, 0);
                if (cfg->pipeline_depth > MAX_PIPELINE_DEPTH) {
                    cfg->pipeline_depth = MAX_PIPELINE_DEPTH;
                }
                if (cfg->pipeline_depth == 0) {
                    cfg->pipeline_depth = 1;
                }
                break;
            case 'E':
                cfg->eid_idx = (uint16_t)strtoul(optarg, NULL, 0);
                break;
            case 'M':
                cfg->trans_mode = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'P':
                cfg->tp_type = (umq_tp_type_t)strtoul(optarg, NULL, 0);
                break;
            case 'F':
                cfg->enable_flow_control = true;
                break;
            case 'G':
                cfg->enable_flow_control = false;
                break;
            case 'A':
                cfg->use_atomic_window = true;
                break;
            case 'I':
                cfg->initial_credit = (uint16_t)strtoul(optarg, NULL, 0);
                break;
            case 'L':
                cfg->max_credits_request = (uint16_t)strtoul(optarg, NULL, 0);
                break;
            case 'O':
                cfg->fc_timeout_ms = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'l':
                cfg->use_logic_umq = true;
                break;
            case 't':
                cfg->tp_handle_cnt = (uint32_t)strtoul(optarg, NULL, 0);
                break;
            case 'h':
            default:
                usage();
                return -1;
        }
    }

    if (strlen(cfg->dev_name) == 0) {
        printf("Error: device name is required\n");
        return -1;
    }

    if (cfg->use_logic_umq && cfg->donor_sub_num > MAX_CLIENT_NUM) {
        printf("Error: --donor-num %u exceeds MAX_CLIENT_NUM %d\n", cfg->donor_sub_num, MAX_CLIENT_NUM);
        return -1;
    }
    if (cfg->is_server && cfg->client_num > MAX_CLIENT_NUM) {
        printf("Error: --num (clients) %u exceeds MAX_CLIENT_NUM %d\n", cfg->client_num, MAX_CLIENT_NUM);
        return -1;
    }

    return 0;
}

static int send_all(int fd, const void *buf, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len) {
        int ret = send(fd, (const char *)buf + sent, len - sent, 0);
        if (ret <= 0) {
            return -1;
        }
        sent += (uint32_t)ret;
    }
    return 0;
}

static int recv_all(int fd, void *buf, uint32_t len)
{
    uint32_t recvd = 0;
    while (recvd < len) {
        int ret = recv(fd, (char *)buf + recvd, len - recvd, 0);
        if (ret <= 0) {
            return -1;
        }
        recvd += (uint32_t)ret;
    }
    return 0;
}

static int send_u32(int fd, uint32_t val)
{
    uint32_t net_val = htonl(val);
    return send_all(fd, &net_val, sizeof(net_val));
}

static int recv_u32(int fd, uint32_t *val)
{
    uint32_t net_val;
    if (recv_all(fd, &net_val, sizeof(net_val)) != 0) {
        return -1;
    }
    *val = ntohl(net_val);
    return 0;
}

static int send_bind_info(int fd, uint64_t umqh, const char *dev_name, uint16_t eid_idx)
{
    if (send_u32(fd, (uint32_t)strlen(dev_name)) != 0) {
        return -1;
    }
    if (send_all(fd, dev_name, (uint32_t)strlen(dev_name)) != 0) {
        return -1;
    }
    if (send_u32(fd, eid_idx) != 0) {
        return -1;
    }

    uint8_t bind_info[UMQ_MAX_BIND_INFO_SIZE];
    uint32_t bind_info_size = umq_bind_info_get(umqh, bind_info, UMQ_MAX_BIND_INFO_SIZE);
    if (bind_info_size == 0) {
        printf("umq_bind_info_get failed\n");
        return -1;
    }
    if (send_u32(fd, bind_info_size) != 0) {
        return -1;
    }
    if (send_all(fd, bind_info, bind_info_size) != 0) {
        return -1;
    }
    return 0;
}

static int recv_bind_info(int fd, char *dev_name, uint16_t *eid_idx,
                          uint8_t *bind_info, uint32_t *bind_info_size)
{
    uint32_t name_len;
    if (recv_u32(fd, &name_len) != 0 || name_len >= 64) {
        return -1;
    }
    if (recv_all(fd, dev_name, name_len) != 0) {
        return -1;
    }
    dev_name[name_len] = '\0';

    uint32_t eid;
    if (recv_u32(fd, &eid) != 0) {
        return -1;
    }
    *eid_idx = (uint16_t)eid;

    uint32_t bsize;
    if (recv_u32(fd, &bsize) != 0 || bsize > UMQ_MAX_BIND_INFO_SIZE) {
        return -1;
    }
    if (recv_all(fd, bind_info, bsize) != 0) {
        return -1;
    }
    *bind_info_size = bsize;
    return 0;
}

static int create_tcp_server(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct timeval tv;
    tv.tv_sec = SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("bind failed, ip %s port %u errno %d\n", ip, port, errno);
        close(fd);
        return -1;
    }

    if (listen(fd, MAX_CLIENT_NUM) != 0) {
        printf("listen failed\n");
        close(fd);
        return -1;
    }

    printf("Server listening on %s:%u\n", ip, port);
    return fd;
}

static int create_tcp_client(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("connect to %s:%u failed, errno %d\n", ip, port, errno);
        close(fd);
        return -1;
    }

    printf("Connected to server %s:%u\n", ip, port);
    return fd;
}

static int init_umq(m2o_config_t *cfg, bool is_server)
{
    umq_init_cfg_t *init_cfg = (umq_init_cfg_t *)calloc(1, sizeof(*init_cfg));
    if (init_cfg == NULL) {
        return -1;
    }

    init_cfg->buf_mode = cfg->buf_mode;
    init_cfg->feature = UMQ_FEATURE_API_PRO;
    if (cfg->enable_flow_control) {
        init_cfg->feature |= UMQ_FEATURE_ENABLE_FLOW_CONTROL;
    }
    init_cfg->flow_control.use_atomic_window = cfg->use_atomic_window;
    init_cfg->flow_control.initial_credit = cfg->initial_credit;
    init_cfg->flow_control.max_credits_request = cfg->max_credits_request;
    init_cfg->flow_control.timeout_ms = cfg->fc_timeout_ms;
    init_cfg->headroom_size = 0;
    init_cfg->io_lock_free = true;
    init_cfg->trans_info_num = 1;
    init_cfg->trans_info[0].trans_mode = (umq_trans_mode_t)cfg->trans_mode;
    init_cfg->trans_info[0].dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    memcpy(init_cfg->trans_info[0].dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
    init_cfg->trans_info[0].dev_info.dev.eid_idx = cfg->eid_idx;

    if (umq_init(init_cfg) != UMQ_SUCCESS) {
        printf("umq_init failed\n");
        free(init_cfg);
        return -1;
    }

    free(init_cfg);
    return 0;
}

static uint64_t create_main_umq(m2o_config_t *cfg, bool is_server)
{
    uint32_t create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
                           UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH |
                           UMQ_CREATE_FLAG_QUEUE_MODE | UMQ_CREATE_FLAG_TP_MODE |
                           UMQ_CREATE_FLAG_TP_TYPE | UMQ_CREATE_FLAG_MAIN_UMQ | UMQ_CREATE_FLAG_MAX_SHARE_QUEUE_NUM;
    umq_tp_mode_t tp_mode = UMQ_TM_RC;
    if (cfg->use_logic_umq) {
        // Logic UMQ mode: Main UMQ owns the global jetty pool, requires tp_mode=RM
        create_flag |= UMQ_CREATE_FLAG_SHARE_TRANSPORT;
        tp_mode = UMQ_TM_RM;
    }

    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = create_flag,
        .rx_buf_size = cfg->buf_size,
        .tx_buf_size = cfg->buf_size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_INTERRUPT,
        .tp_mode = tp_mode,
        .tp_type = cfg->tp_type,
        .max_share_queue_num = 1024 * 48 + 1,
    };

    sprintf(option.name, "%s", is_server ? "srv_main" : "cli_main");
    option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    memcpy(option.dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
    option.dev_info.dev.eid_idx = cfg->eid_idx;

    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;
    if (strstr(cfg->dev_name, "bond") != NULL) {
        option.create_flag |= UMQ_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    uint64_t umqh = umq_create(&option);
    if (umqh == UMQ_INVALID_HANDLE) {
        printf("create main umq failed\n");
        return UMQ_INVALID_HANDLE;
    }

    printf("Created main UMQ: 0x%lx (mode=%s, tp_mode=%s)\n", umqh,
        cfg->use_logic_umq ? "Logic" : "Sub", tp_mode == UMQ_TM_RM ? "RM" : "RC");
    return umqh;
}

static uint64_t create_logic_umq_tmp(m2o_config_t *cfg, uint64_t main_umqh, uint64_t umq_ctx_val)
{
    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
                       UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH |
                       UMQ_CREATE_FLAG_QUEUE_MODE | UMQ_CREATE_FLAG_TP_MODE |
                       UMQ_CREATE_FLAG_TP_TYPE |
                       UMQ_CREATE_FLAG_SHARE_RQ | UMQ_CREATE_FLAG_SHARE_TRANSPORT |
                       UMQ_CREATE_FLAG_UMQ_CTX,
        .rx_buf_size = cfg->buf_size,
        .tx_buf_size = cfg->buf_size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_INTERRUPT,
        .tp_mode = UMQ_TM_RM,   // Logic UMQ shares FC jfr with Main; tp_mode aligned with Main
        .tp_type = cfg->tp_type,
        .share_rq_umqh = main_umqh,
        .umq_ctx = umq_ctx_val,
    };

    char dev_name[] = "bonding_dev_1";

    sprintf(option.name, "%s_logic_%u", "cli", (uint32_t)umq_ctx_val);
    option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    memcpy(option.dev_info.dev.dev_name, dev_name, strlen(dev_name));
    option.dev_info.dev.eid_idx = 0;

    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;
    if (strstr(dev_name, "bond") != NULL) {
        option.create_flag |= UMQ_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    uint64_t umqh = umq_create(&option);
    if (umqh == UMQ_INVALID_HANDLE) {
        printf("create logic umq failed\n");
        return UMQ_INVALID_HANDLE;
    }

    printf("Created logic UMQ: 0x%lx (main=0x%lx, ctx=0x%lx)\n", umqh, main_umqh, umq_ctx_val);
    return umqh;
}

static uint64_t create_logic_umq(m2o_config_t *cfg, uint64_t main_umqh,
                                 uint64_t umq_ctx_val, bool is_server,
                                 const char *dev_name, uint16_t eid_idx)
{
    // Logic UMQ: shares Main's jfr, borrows jetty from pool at first post.
    // No SUB_UMQ / MAIN_UMQ flag; only SHARE_RQ + SHARE_TRANSPORT.
    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
                       UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH |
                       UMQ_CREATE_FLAG_QUEUE_MODE | UMQ_CREATE_FLAG_TP_MODE |
                       UMQ_CREATE_FLAG_TP_TYPE |
                       UMQ_CREATE_FLAG_SHARE_RQ | UMQ_CREATE_FLAG_SHARE_TRANSPORT |
                       UMQ_CREATE_FLAG_UMQ_CTX,
        .rx_buf_size = cfg->buf_size,
        .tx_buf_size = cfg->buf_size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_INTERRUPT,
        .tp_mode = UMQ_TM_RM,   // Logic UMQ shares FC jfr with Main; tp_mode aligned with Main
        .tp_type = cfg->tp_type,
        .share_rq_umqh = main_umqh,
        .umq_ctx = umq_ctx_val,
    };

    sprintf(option.name, "%s_logic_%u", is_server ? "srv" : "cli", (uint32_t)umq_ctx_val);
    option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    memcpy(option.dev_info.dev.dev_name, dev_name, strlen(dev_name));
    option.dev_info.dev.eid_idx = eid_idx;

    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;
    if (strstr(dev_name, "bond") != NULL) {
        option.create_flag |= UMQ_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    uint64_t umqh = umq_create(&option);
    if (umqh == UMQ_INVALID_HANDLE) {
        printf("create logic umq failed\n");
        return UMQ_INVALID_HANDLE;
    }

    printf("Created logic UMQ: 0x%lx (main=0x%lx, ctx=0x%lx)\n", umqh, main_umqh, umq_ctx_val);
    return umqh;
}

static uint64_t create_sub_umq(m2o_config_t *cfg, uint64_t main_umqh,
                               uint64_t umq_ctx_val, bool is_server,
                               const char *dev_name, uint16_t eid_idx)
{
    // Plain Sub UMQ mode (no Logic). For --logic-umq use create_donor_sub_umq + create_logic_umq.
    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
                       UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH |
                       UMQ_CREATE_FLAG_QUEUE_MODE | UMQ_CREATE_FLAG_TP_MODE |
                       UMQ_CREATE_FLAG_TP_TYPE |
                       UMQ_CREATE_FLAG_SUB_UMQ | UMQ_CREATE_FLAG_SHARE_RQ |
                       UMQ_CREATE_FLAG_UMQ_CTX,
        .rx_buf_size = cfg->buf_size,
        .tx_buf_size = cfg->buf_size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_INTERRUPT,
        .tp_mode = UMQ_TM_RC,
        .tp_type = cfg->tp_type,
        .share_rq_umqh = main_umqh,
        .umq_ctx = umq_ctx_val,
    };

    sprintf(option.name, "%s_sub_%u", is_server ? "srv" : "cli", (uint32_t)umq_ctx_val);
    option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
    memcpy(option.dev_info.dev.dev_name, dev_name, strlen(dev_name));
    option.dev_info.dev.eid_idx = eid_idx;

    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;
    if (strstr(dev_name, "bond") != NULL) {
        option.create_flag |= UMQ_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    uint64_t umqh = umq_create(&option);
    if (umqh == UMQ_INVALID_HANDLE) {
        printf("create sub umq failed\n");
        return UMQ_INVALID_HANDLE;
    }

    printf("Created sub UMQ: 0x%lx (main=0x%lx, ctx=0x%lx)\n", umqh, main_umqh, umq_ctx_val);
    return umqh;
}

static int post_rx_buffers(uint64_t umqh, uint32_t buf_size, uint32_t count)
{
    uint32_t need_post = count * 4;
    umq_io_option_t io_rx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_RX,
    };

    while (need_post > 0) {
        uint32_t batch = need_post > MAX_POLL_BATCH ? MAX_POLL_BATCH : need_post;
        umq_buf_t *buf = umq_buf_alloc(buf_size, batch, umqh, NULL);
        if (buf == NULL) {
            printf("alloc rx buf failed\n");
            return -1;
        }
        umq_buf_t *bad_buf = NULL;
        if (umq_post(umqh, buf, &io_rx_ops, &bad_buf) != UMQ_SUCCESS) {
            printf("post rx failed\n");
            umq_buf_free(bad_buf);
            return -1;
        }
        need_post -= batch;
    }
    return 0;
}

static int wait_umq_ready(uint64_t umqh)
{
    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_TX,
    };

    int poll_cnt = 0;
    do {
        umq_buf_t *buf = NULL;
        int ret = umq_poll(umqh, &io_tx_ops, &buf, 1);
        if (ret != 0) {
            break;
        }
        umq_state_t state = umq_state_get(umqh);
        if (state != QUEUE_STATE_IDLE) {
            break;
        }
        usleep(WAIT_UMQ_READY_US);
    } while (poll_cnt++ < WAIT_UMQ_READY_ROUND);

    if (umq_state_get(umqh) != QUEUE_STATE_READY) {
        printf("wait umq ready failed, state %d\n", umq_state_get(umqh));
        return -1;
    }
    return 0;
}

static void set_pro_send_data(umq_buf_t *buf, uint32_t msg_size)
{
    while (buf) {
        umq_buf_pro_t *pro = (umq_buf_pro_t *)buf->qbuf_ext;
        pro->flag.value = 0;
        pro->flag.bs.solicited_enable = 1;
        pro->flag.bs.complete_enable = 1;
        if (msg_size < INLINE_LIMIT_SIZE) {
            pro->flag.bs.inline_flag = INLINE_ENABLE;
        }
        pro->opcode = UMQ_OPC_SEND;
        buf = buf->qbuf_next;
    }
}

static bool spsc_push(umq_buf_t **bufs, atomic_uint *head, atomic_uint *tail,
                       uint32_t size, umq_buf_t *buf)
{
    uint32_t t = atomic_load_explicit(tail, memory_order_relaxed);
    uint32_t next = (t + 1) % size;
    if (next == atomic_load_explicit(head, memory_order_acquire)) {
        return false;
    }
    bufs[t] = buf;
    atomic_store_explicit(tail, next, memory_order_release);
    return true;
}

static umq_buf_t *spsc_pop(umq_buf_t **bufs, atomic_uint *head, atomic_uint *tail,
                            uint32_t size)
{
    uint32_t h = atomic_load_explicit(head, memory_order_relaxed);
    if (h == atomic_load_explicit(tail, memory_order_acquire)) {
        return NULL;
    }
    umq_buf_t *buf = bufs[h];
    atomic_store_explicit(head, (h + 1) % size, memory_order_release);
    return buf;
}

static bool dispatch_push(m2o_sub_umq_info_t *sub, umq_buf_t *buf)
{
    return spsc_push(sub->dispatch_bufs, &sub->dispatch_head,
                     &sub->dispatch_tail, SPSC_QUEUE_SIZE, buf);
}

static umq_buf_t *dispatch_pop(m2o_sub_umq_info_t *sub)
{
    return spsc_pop(sub->dispatch_bufs, &sub->dispatch_head,
                    &sub->dispatch_tail, SPSC_QUEUE_SIZE);
}

static bool worker_poll_fc_credit(m2o_sub_umq_info_t *sub)
{
    uint64_t sub_umqh = sub->umqh;
    umq_buf_t *fc_buf = NULL;
    umq_io_option_t io_rx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_RX,
    };
    int ret = umq_poll(sub_umqh, &io_rx_ops, &fc_buf, 1);
    if (ret > 0 && fc_buf) {
        bool has_credit = (fc_buf->status == UMQ_FAKE_BUF_FC_UPDATE);
        umq_buf_free(fc_buf);
        return has_credit;
    }
    return false;
}

static bool pending_push(m2o_sub_umq_info_t *sub, umq_buf_t *buf)
{
    if (sub->pending_count >= sub->pending_capacity) {
        return false;
    }
    sub->pending_bufs[sub->pending_tail] = buf;
    sub->pending_tail = (sub->pending_tail + 1) % sub->pending_capacity;
    sub->pending_count++;
    return true;
}

static umq_buf_t *pending_pop(m2o_sub_umq_info_t *sub)
{
    if (sub->pending_count == 0) {
        return NULL;
    }
    umq_buf_t *buf = sub->pending_bufs[sub->pending_head];
    sub->pending_head = (sub->pending_head + 1) % sub->pending_capacity;
    sub->pending_count--;
    return buf;
}

static void tx_free_push(m2o_sub_umq_info_t *sub, umq_buf_t *buf)
{
    if (sub->tx_free_count < sub->tx_free_capacity) {
        sub->tx_free_bufs[sub->tx_free_count++] = buf;
    }
}

static umq_buf_t *tx_free_pop(m2o_sub_umq_info_t *sub)
{
    if (sub->tx_free_count == 0) {
        return NULL;
    }
    return sub->tx_free_bufs[--sub->tx_free_count];
}

static void worker_reclaim_tx_bufs(m2o_sub_umq_info_t *sub)
{
    umq_buf_t *tx_cqes[MAX_POLL_BATCH];
    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_TX,
    };
    int tx_cnt = umq_poll(sub->umqh, &io_tx_ops, tx_cqes, MAX_POLL_BATCH);
    for (int i = 0; i < tx_cnt; i++) {
        if (tx_cqes[i]->status == UMQ_FAKE_BUF_FC_UPDATE) {
            umq_buf_free(tx_cqes[i]);
        } else {
            tx_free_push(sub, tx_cqes[i]);
        }
    }
}

static void worker_drain_pending(m2o_sub_umq_info_t *sub, server_ctx_t *ctx,
                                 uint64_t *local_send, uint64_t *local_recv)
{
    // umq_buf_t *resp_buf = NULL;
    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_TX,
    };
    while (sub->pending_count > 0 && !sub->worker_stop && !g_force_quit) {
        umq_buf_t *resp_buf = tx_free_pop(sub);
        if (resp_buf == NULL) {
            worker_reclaim_tx_bufs(sub);
            resp_buf = tx_free_pop(sub);
            if (resp_buf == NULL) {
                break;
            }
        }

        umq_buf_t *rx_buf = sub->pending_bufs[sub->pending_head];
        umq_buf_t *bad_buf = NULL;
        int ret = umq_post(sub->umqh, resp_buf, &io_tx_ops, &bad_buf);
        if (ret != UMQ_SUCCESS) {
            if (bad_buf != NULL && bad_buf != resp_buf) {
                umq_buf_split(resp_buf, bad_buf);
                umq_buf_free(bad_buf);
            }
            tx_free_push(sub, resp_buf);
            if (ret == -UMQ_ERR_EAGAIN) {
                break;
            }
            sub->pending_head = (sub->pending_head + 1) % sub->pending_capacity;
            sub->pending_count--;
            umq_buf_free(rx_buf);
            continue;
        }
        (*local_send)++;

        sub->pending_head = (sub->pending_head + 1) % sub->pending_capacity;
        sub->pending_count--;
        umq_buf_free(rx_buf);
        (*local_recv)++;
    }
}

static bool worker_try_send(m2o_sub_umq_info_t *sub, server_ctx_t *ctx, umq_buf_t *rx_buf,
                             uint64_t *local_send, uint64_t *local_recv)
{
    umq_buf_t *resp_buf;
    // = tx_free_pop(sub);
    // if (resp_buf == NULL) {
    //     worker_reclaim_tx_bufs(sub);
    //     resp_buf = tx_free_pop(sub);
    //     if (resp_buf == NULL) {
    //         printf("=======tx_free_pop failed=======\n");
    //         return false;
    //     }
    // }
    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_TX,
    };

    do {
        resp_buf = tx_free_pop(sub);
    } while (resp_buf == NULL);

    umq_buf_t *bad_buf = NULL;

RETRY:
    int ret = umq_post(sub->umqh, resp_buf, &io_tx_ops, &bad_buf);
    if (ret != UMQ_SUCCESS) {
        if (bad_buf != NULL && bad_buf != resp_buf) {
            umq_buf_split(resp_buf, bad_buf);
            umq_buf_free(bad_buf);
        }
        tx_free_push(sub, resp_buf);
        if (ret == -UMQ_ERR_EAGAIN) {
            return false;
        }

        if (ret == -UMQ_ERR_EMLINK || ret == -UMQ_ERR_ETOOMANYREFS || ret == -UMQ_ERR_ENOBUFS) {
            usleep(10);
            goto RETRY;
        }

        printf("=======umq post fialed ret %d=======\n", ret);
        umq_buf_free(rx_buf);
        return true;
    }
    (*local_send)++;
    umq_buf_free(rx_buf);
    (*local_recv)++;
    return true;
}

// Logic UMQ mode: dedicated poller for donor Sub UMQ TX completions.
// Without this, the Sub UMQ's poll_tx never runs, so tx_outstanding never
// reaches 0 and the exchange protocol can never reclaim the borrowed jetty node.
static void *server_poll_sub_tx_func(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_TX,
    };

    // umq_interrupt_fd_list_t fd_list;
    // umq_interrupt_option_t fd_list_option = {
    //     .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
    //     .direction = UMQ_IO_TX,
    // };
    // if (umq_interrupt_fd_list_get(ctx->main_umqh, &fd_list_option, &fd_list) != UMQ_SUCCESS) {
    //     printf("umq_interrupt_fd_list_get failed\n");
    //     return NULL;
    // }

    // printf("umq_interrupt_fd_list_get fd cnt %u, fd %d\n", fd_list.fd_num, fd_list.fd[0]);

    while (!ctx->force_quit && !g_force_quit && !ctx->poller_stop) {
        // bool did_work = false;
        for (uint32_t i = 0; i < ctx->tp_handle_cnt; i++) {
            umq_buf_t *cqes[MAX_POLL_BATCH];
            io_tx_ops.tp_handle_idx = ctx->tp_handle_idx[i];
            int cnt = umq_poll(ctx->main_umqh, &io_tx_ops, cqes, MAX_POLL_BATCH);
            for (int j = 0; j < cnt; j++) {
                umq_buf_pro_t *pro = (umq_buf_pro_t *)cqes[j]->qbuf_ext;
                m2o_umq_ctx_t *umq_ctx = (m2o_umq_ctx_t *)(uintptr_t)pro->umq_ctx;
                tx_free_push(&ctx->sub_umqs[umq_ctx->client_idx], cqes[j]);
                // umq_buf_free(cqes[j]);
            }
        }
    }
    return NULL;
}

static void *server_worker_func(void *arg)
{
    m2o_sub_umq_info_t *sub = (m2o_sub_umq_info_t *)arg;
    server_ctx_t *ctx = sub->srv_ctx;
    uint64_t local_send = 0;
    uint64_t local_recv = 0;

    while (!sub->worker_stop && !g_force_quit) {
        sem_wait(&sub->dispatch_sem);
        if (sub->worker_stop) {
            break;
        }

        umq_buf_t *dispatched_buf;
        while ((dispatched_buf = dispatch_pop(sub)) != NULL) {
            if (dispatched_buf->status == UMQ_FAKE_BUF_FC_MSG) {
                umq_buf_free(dispatched_buf);
                bool has_credit = worker_poll_fc_credit(sub);
                if (has_credit) {
                    worker_drain_pending(sub, ctx, &local_send, &local_recv);
                }
                continue;
            }

            if (sub->pending_count > 0) {
                if (!pending_push(sub, dispatched_buf)) {
                    umq_buf_free(dispatched_buf);
                }
                continue;
            }

            if (!worker_try_send(sub, ctx, dispatched_buf, &local_send, &local_recv)) {
                if (!pending_push(sub, dispatched_buf)) {
                    umq_buf_free(dispatched_buf);
                }
            }
        }
    }

    atomic_fetch_add(&ctx->total_send_cnt, local_send);
    atomic_fetch_add(&ctx->total_recv_cnt, local_recv);

    while (sub->pending_count > 0) {
        umq_buf_t *buf = pending_pop(sub);
        umq_buf_free(buf);
    }
    return NULL;
}

static int sub_umq_init_queues(m2o_sub_umq_info_t *sub, uint64_t sub_umqh,
                               uint32_t msg_size, uint32_t tx_depth)
{
    sub->dispatch_bufs = (umq_buf_t **)calloc(SPSC_QUEUE_SIZE, sizeof(umq_buf_t *));
    if (sub->dispatch_bufs == NULL) {
        return -1;
    }
    atomic_init(&sub->dispatch_head, 0);
    atomic_init(&sub->dispatch_tail, 0);
    if (sem_init(&sub->dispatch_sem, 0, 0) != 0) {
        free(sub->dispatch_bufs);
        sub->dispatch_bufs = NULL;
        return -1;
    }

    sub->pending_capacity = SPSC_QUEUE_SIZE;
    sub->pending_bufs = (umq_buf_t **)calloc(sub->pending_capacity, sizeof(umq_buf_t *));
    if (sub->pending_bufs == NULL) {
        sem_destroy(&sub->dispatch_sem);
        free(sub->dispatch_bufs);
        sub->dispatch_bufs = NULL;
        return -1;
    }
    sub->pending_head = 0;
    sub->pending_tail = 0;
    sub->pending_count = 0;

    sub->tx_free_capacity = tx_depth;
    sub->tx_free_bufs = (umq_buf_t **)calloc(sub->tx_free_capacity, sizeof(umq_buf_t *));
    if (sub->tx_free_bufs == NULL) {
        free(sub->pending_bufs);
        sub->pending_bufs = NULL;
        sem_destroy(&sub->dispatch_sem);
        free(sub->dispatch_bufs);
        sub->dispatch_bufs = NULL;
        return -1;
    }
    sub->tx_free_count = 0;
    for (uint32_t i = 0; i < tx_depth; i++) {
        umq_buf_t *buf = umq_buf_alloc(msg_size, 1, sub_umqh, NULL);
        if (buf == NULL) {
            printf("alloc tx free buf %u failed\n", i);
            for (uint32_t j = 0; j < sub->tx_free_count; j++) {
                umq_buf_free(sub->tx_free_bufs[j]);
            }
            free(sub->tx_free_bufs);
            sub->tx_free_bufs = NULL;
            free(sub->pending_bufs);
            sub->pending_bufs = NULL;
            sem_destroy(&sub->dispatch_sem);
            free(sub->dispatch_bufs);
            sub->dispatch_bufs = NULL;
            return -1;
        }
        set_pro_send_data(buf, msg_size);
        tx_free_push(sub, buf);
    }
    return 0;
}

static void sub_umq_destroy_queues(m2o_sub_umq_info_t *sub)
{
    if (sub->dispatch_bufs) {
        free(sub->dispatch_bufs);
        sub->dispatch_bufs = NULL;
    }
    if (sub->pending_bufs) {
        free(sub->pending_bufs);
        sub->pending_bufs = NULL;
    }
    for (uint32_t i = 0; i < sub->tx_free_count; i++) {
        umq_buf_free(sub->tx_free_bufs[i]);
    }
    if (sub->tx_free_bufs) {
        free(sub->tx_free_bufs);
        sub->tx_free_bufs = NULL;
    }
    sem_destroy(&sub->dispatch_sem);
}

static int run_server(m2o_config_t *cfg)
{
    server_ctx_t *ctx = (server_ctx_t *)calloc(1, sizeof(server_ctx_t));
    if (ctx == NULL) {
        printf("alloc server ctx failed\n");
        return -1;
    }
    ctx->cfg = cfg;
    ctx->poller_stop = false;
    ctx->epoll_fd = -1;
    atomic_init(&ctx->total_recv_cnt, 0);
    atomic_init(&ctx->total_send_cnt, 0);

    if (init_umq(cfg, true) != 0) {
        printf("init umq failed\n");
        goto FREE_CTX;
    }

    ctx->main_umqh = create_main_umq(cfg, true);
    if (ctx->main_umqh == UMQ_INVALID_HANDLE) {
        goto UNINIT;
    }

    static uint64_t logic_umqh_list[16386];
    for (uint64_t i = 0; i < 16386; i++) {
        logic_umqh_list[i] = create_logic_umq_tmp(cfg, ctx->main_umqh, i);
        if (logic_umqh_list[i] == 0) {
            printf("create_logic_umq_tmp failed\n");
             goto UNINIT;
        }
    }


    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;

    umq_tp_resource_create_option_t option = { 0 };
    if (strstr(cfg->dev_name, "bond") != NULL) {
        option.create_flag = UMQ_TP_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    if (cfg->use_logic_umq) {
        ctx->tp_handle_cnt = cfg->tp_handle_cnt;
        for (uint32_t i = 0; i < cfg->tp_handle_cnt; i++) {
            if ((ctx->tp_handle_idx[i] = umq_transport_pool_resource_create(ctx->main_umqh, &option)) > 10240) {
                printf("umq_expand_tp_resource failed\n");
                goto UNINIT;
            }
        }
        
    }

    int listen_fd = create_tcp_server(cfg->server_ip[0] ? cfg->server_ip : "0.0.0.0", cfg->tcp_port);
    if (listen_fd < 0) {
        goto DESTROY_MAIN;
    }

    umq_buf_alloc(0,1,0,NULL);

    printf("Waiting for %u clients to connect...\n", cfg->client_num);
    for (uint32_t i = 0; i < cfg->client_num; i++) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            printf("accept failed, errno %d\n", errno);
            goto CLEANUP_CLIENTS;
        }
        printf("Client %u connected (fd=%d)\n", i, client_fd);

        char cli_dev_name[64] = {0};
        uint16_t cli_eid_idx = 0;
        uint8_t cli_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
        uint32_t cli_bind_size = 0;

        if (recv_bind_info(client_fd, cli_dev_name, &cli_eid_idx, cli_bind_info, &cli_bind_size) != 0) {
            printf("recv client %u bind info failed\n", i);
            close(client_fd);
            goto CLEANUP_CLIENTS;
        }
        printf("Client %u: dev=%s eid_idx=%u bind_size=%u\n", i, cli_dev_name, cli_eid_idx, cli_bind_size);

        m2o_umq_ctx_t *umq_ctx = (m2o_umq_ctx_t *)malloc(sizeof(m2o_umq_ctx_t));
        if (umq_ctx == NULL) {
            close(client_fd);
            goto CLEANUP_CLIENTS;
        }
        umq_ctx->client_idx = i;
        umq_ctx->sub_umqh = 0;

        // Logic mode: create a hidden donor Sub UMQ first (registers jetty pair in pool).
        // The per-client UMQ for communication is the Logic UMQ.
        // if (cfg->use_logic_umq) {
        //     uint32_t donor_idx = (cfg->donor_sub_num > 0) ? cfg->donor_sub_num - 1 - i : i;
        //     // Use server's own dev/eid_idx for the donor so the pool stays on the local device.
        //     uint64_t donor_umqh = create_donor_sub_umq(cfg, ctx->main_umqh, donor_idx,
        //                                                cfg->dev_name, cfg->eid_idx);
        //     if (donor_umqh == UMQ_INVALID_HANDLE) {
        //         free(umq_ctx);
        //         close(client_fd);
        //         goto CLEANUP_CLIENTS;
        //     }
        //     ctx->donor_umqhs[i] = donor_umqh;
        // }

        uint64_t sub_umqh;
        if (cfg->use_logic_umq) {
            sub_umqh = create_logic_umq(cfg, ctx->main_umqh, (uint64_t)(uintptr_t)umq_ctx,
                                        true, cli_dev_name, cli_eid_idx);
        } else {
            sub_umqh = create_sub_umq(cfg, ctx->main_umqh, (uint64_t)(uintptr_t)umq_ctx,
                                      true, cli_dev_name, cli_eid_idx);
        }
        if (sub_umqh == UMQ_INVALID_HANDLE) {
            if (cfg->use_logic_umq) {
                ctx->donor_umqhs[i] = UMQ_INVALID_HANDLE;
            }
            free(umq_ctx);
            close(client_fd);
            goto CLEANUP_CLIENTS;
        }
        umq_ctx->sub_umqh = sub_umqh;

        if (send_bind_info(client_fd, sub_umqh, cli_dev_name, cli_eid_idx) != 0) {
            printf("send server %s bind info to client %u failed\n",
                cfg->use_logic_umq ? "logic" : "sub", i);
            umq_destroy(sub_umqh);
            if (cfg->use_logic_umq) {
                ctx->donor_umqhs[i] = UMQ_INVALID_HANDLE;
            }
            free(umq_ctx);
            close(client_fd);
            goto CLEANUP_CLIENTS;
        }

        if (umq_bind(sub_umqh, cli_bind_info, cli_bind_size) != UMQ_SUCCESS) {
            printf("bind server %s umq for client %u failed\n",
                cfg->use_logic_umq ? "logic" : "sub", i);
            umq_destroy(sub_umqh);
            if (cfg->use_logic_umq) {
                ctx->donor_umqhs[i] = UMQ_INVALID_HANDLE;
            }
            free(umq_ctx);
            close(client_fd);
            goto CLEANUP_CLIENTS;
        }
        printf("Server %s UMQ for client %u bound successfully\n",
            cfg->use_logic_umq ? "logic" : "sub", i);

        ctx->sub_umqs[i].umqh = sub_umqh;
        ctx->sub_umqs[i].umq_ctx = umq_ctx;
        ctx->sub_umqs[i].client_fd = client_fd;
        ctx->sub_umqs[i].valid = true;
        ctx->sub_umqs[i].srv_ctx = ctx;
        ctx->sub_umqs[i].worker_stop = false;

        if (sub_umq_init_queues(&ctx->sub_umqs[i], sub_umqh, cfg->buf_size,
                                cfg->tx_depth) != 0) {
            printf("init queues for %s umq %u failed\n",
                cfg->use_logic_umq ? "logic" : "sub", i);
            umq_unbind(sub_umqh);
            umq_destroy(sub_umqh);
            free(umq_ctx);
            close(client_fd);
            ctx->sub_umqs[i].valid = false;
            goto CLEANUP_CLIENTS;
        }

        ctx->client_cnt++;
    }

    if (post_rx_buffers(ctx->main_umqh, cfg->buf_size, cfg->rx_depth) != 0) {
        printf("post rx buffers failed\n");
        goto CLEANUP_CLIENTS;
    }
    printf("Posted %u RX buffers to main UMQ\n", cfg->rx_depth);

    for (uint32_t i = 0; i < cfg->client_num; i++) {
        if (wait_umq_ready(ctx->sub_umqs[i].umqh) != 0) {
            printf("wait %s umq %u ready failed\n",
                cfg->use_logic_umq ? "logic" : "sub", i);
        }
    }

    for (uint32_t i = 0; i < cfg->client_num; i++) {
        if (send_u32(ctx->sub_umqs[i].client_fd, 0) != 0) {
            printf("send sync to client %u failed\n", i);
        }
    }

    for (uint32_t i = 0; i < cfg->client_num; i++) {
        if (pthread_create(&ctx->sub_umqs[i].worker_thread, NULL,
                           server_worker_func, &ctx->sub_umqs[i]) != 0) {
            printf("create worker thread for %s umq %u failed\n",
                cfg->use_logic_umq ? "logic" : "sub", i);
            ctx->force_quit = true;
            for (uint32_t j = 0; j < i; j++) {
                ctx->sub_umqs[j].worker_stop = true;
                sem_post(&ctx->sub_umqs[j].dispatch_sem);
            }
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(ctx->sub_umqs[j].worker_thread, NULL);
            }
            goto CLEANUP_QUEUES;
        }
    }
    if (cfg->use_logic_umq) {
        ctx->poller_stop = false;
        if (pthread_create(&ctx->sub_tx_poller[0], NULL, server_poll_sub_tx_func, ctx) != 0) {
            printf("create sub tx poller thread failed\n");
            ctx->force_quit = true;
            for (uint32_t j = 0; j < cfg->client_num; j++) {
                ctx->sub_umqs[j].worker_stop = true;
                sem_post(&ctx->sub_umqs[j].dispatch_sem);
            }
            for (uint32_t j = 0; j < cfg->client_num; j++) {
                pthread_join(ctx->sub_umqs[j].worker_thread, NULL);
            }
            goto CLEANUP_QUEUES;
        }
        printf("Sub UMQ TX poller thread started (keeps jetty pool reclaim alive)\n");
    }

    printf("Server started: %u clients, %u dedicated worker threads (1 per %s UMQ), epoll on main UMQ RX fd\n",
           cfg->client_num, cfg->client_num, cfg->use_logic_umq ? "logic" : "sub");

    umq_interrupt_option_t rx_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_RX,
    };

    ctx->main_rx_fd = umq_interrupt_fd_get(ctx->main_umqh, &rx_option);
    if (ctx->main_rx_fd < 0) {
        printf("get main umq rx interrupt fd failed\n");
        ctx->force_quit = true;
        if (cfg->use_logic_umq) { ctx->poller_stop = true; pthread_join(ctx->sub_tx_poller[0], NULL); }
        goto STOP_WORKERS;
    }
    printf("Main UMQ RX interrupt fd: %d\n", ctx->main_rx_fd);

    if (umq_rearm_interrupt(ctx->main_umqh, false, &rx_option) != UMQ_SUCCESS) {
        printf("rearm main umq rx interrupt failed\n");
        ctx->force_quit = true;
        if (cfg->use_logic_umq) { ctx->poller_stop = true; pthread_join(ctx->sub_tx_poller[0], NULL); }
        goto STOP_WORKERS;
    }

    ctx->epoll_fd = epoll_create(1);
    if (ctx->epoll_fd < 0) {
        printf("epoll_create failed\n");
        if (cfg->use_logic_umq) { ctx->poller_stop = true; pthread_join(ctx->sub_tx_poller[0], NULL); }
        goto STOP_WORKERS;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = ctx->main_rx_fd;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->main_rx_fd, &ev) != 0) {
        printf("epoll_ctl add failed, errno %d\n", errno);
        close(ctx->epoll_fd);
        if (cfg->use_logic_umq) { ctx->poller_stop = true; pthread_join(ctx->sub_tx_poller[0], NULL); }
        goto CLEANUP_QUEUES;
    }

    struct timespec cpu_start, cpu_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    uint64_t cycle_start = get_cycles();
    double cpu_mhz = get_cpu_mhz(true);

    struct epoll_event events[1];

    umq_io_option_t io_rx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_RX,
    };

    while (!g_force_quit && !ctx->force_quit) {
        int nfds = epoll_wait(ctx->epoll_fd, events, 1, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            printf("epoll_wait error, errno %d\n", errno);
            break;
        }
        if (nfds == 0) {
            continue;
        }

        if (umq_get_cq_event(ctx->main_umqh, &rx_option) < 0) {
            umq_rearm_interrupt(ctx->main_umqh, false, &rx_option);
            continue;
        }

        umq_buf_t *rx_bufs[MAX_POLL_BATCH];
        int rx_cnt = umq_poll(ctx->main_umqh, &io_rx_ops, rx_bufs, MAX_POLL_BATCH);
        if (rx_cnt < 0) {
            umq_rearm_interrupt(ctx->main_umqh, false, &rx_option);
            continue;
        }

        do {
            uint32_t dispatch_counts[MAX_CLIENT_NUM] = {0};
            int fc_buf_cnt = 0;
            for (int i = 0; i < rx_cnt; i++) {
                umq_buf_t *buf = rx_bufs[i];
                if (buf->status == UMQ_FAKE_BUF_FC_ERR) {
                    fc_buf_cnt++;
                    umq_buf_free(buf);
                    continue;
                }

                umq_buf_pro_t *pro = (umq_buf_pro_t *)buf->qbuf_ext;
                m2o_umq_ctx_t *umq_ctx = (m2o_umq_ctx_t *)(uintptr_t)pro->umq_ctx;
                uint32_t client_idx = umq_ctx ? umq_ctx->client_idx : UINT32_MAX;

                if (buf->status == UMQ_FAKE_BUF_FC_UPDATE) {
                    umq_buf_free(buf);
                    fc_buf_cnt++;
                    continue;
                } else if (buf->status == UMQ_FAKE_BUF_FC_MSG) {
                    fc_buf_cnt++;
                }

                if (client_idx < ctx->client_cnt && ctx->sub_umqs[client_idx].valid) {
                    if (dispatch_push(&ctx->sub_umqs[client_idx], buf)) {
                        dispatch_counts[client_idx]++;
                    } else {
                        umq_buf_free(buf);
                    }
                } else {
                    umq_buf_free(buf);
                }
            }

            for (uint32_t i = 0; i < ctx->client_cnt; i++) {
                if (dispatch_counts[i] > 0) {
                    sem_post(&ctx->sub_umqs[i].dispatch_sem);
                }
            }

            if (rx_cnt - fc_buf_cnt > 0) {
                umq_buf_t *new_buf = umq_buf_alloc(cfg->buf_size, rx_cnt - fc_buf_cnt, ctx->main_umqh, NULL);
                if (new_buf) {
                    umq_buf_t *bad_buf = NULL;
                    if (umq_post(ctx->main_umqh, new_buf, &io_rx_ops, &bad_buf) != UMQ_SUCCESS) {
                        umq_buf_free(bad_buf);
                    }
                }
            }

            rx_cnt = umq_poll(ctx->main_umqh, &io_rx_ops, rx_bufs, MAX_POLL_BATCH);
        } while (rx_cnt > 0 && !g_force_quit && !ctx->force_quit);

        if (umq_rearm_interrupt(ctx->main_umqh, false, &rx_option) != UMQ_SUCCESS) {
            printf("rearm interrupt failed\n");
            break;
        }
    }

    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        ctx->sub_umqs[i].worker_stop = true;
        sem_post(&ctx->sub_umqs[i].dispatch_sem);
    }
    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        pthread_join(ctx->sub_umqs[i].worker_thread, NULL);
    }

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);
    uint64_t cycle_end = get_cycles();
    double test_duration_us = (double)(cycle_end - cycle_start) / cpu_mhz;
    double test_duration_sec = test_duration_us / PERFTEST_1M;

    double cpu_time_us = (cpu_end.tv_sec - cpu_start.tv_sec) * (double)PERFTEST_1M +
                         (cpu_end.tv_nsec - cpu_start.tv_nsec) / 1000.0;
    double cpu_util = cpu_time_us * PERCENT_NUM / test_duration_us;

    uint64_t total_recv = atomic_load(&ctx->total_recv_cnt);
    uint64_t total_send = atomic_load(&ctx->total_send_cnt);
    double bw = (double)total_send * cfg->buf_size / PERFTEST_1MB / test_duration_sec;
    double mpps = (double)total_send / test_duration_us;

    printf("\n================== Server Performance Results ==================\n");
    printf("  Message size:       %u bytes\n", cfg->buf_size);
    printf("  Number of clients:  %u\n", cfg->client_num);
    printf("  Worker threads:     %u (1 per %s UMQ)\n", cfg->client_num,
        cfg->use_logic_umq ? "logic" : "sub");
    printf("  Test duration:      %.3f s\n", test_duration_sec);
    printf("  Total received:     %lu\n", (unsigned long)total_recv);
    printf("  Total sent:         %lu\n", (unsigned long)total_send);
    printf("  Throughput:         %.3f Mpps\n", mpps);
    printf("  Bandwidth:          %.3f MB/s\n", bw);
    printf("  CPU utilization:    %.2f%%\n", cpu_util);
    printf("================================================================\n");

STOP_WORKERS:
    if (cfg->use_logic_umq) {
        ctx->poller_stop = true;
        pthread_join(ctx->sub_tx_poller[0], NULL);
    }
    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        ctx->sub_umqs[i].worker_stop = true;
        sem_post(&ctx->sub_umqs[i].dispatch_sem);
    }
    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        pthread_join(ctx->sub_umqs[i].worker_thread, NULL);
    }

CLEANUP_QUEUES:
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        sub_umq_destroy_queues(&ctx->sub_umqs[i]);
    }
CLEANUP_CLIENTS:
    // Step 1: destroy per-client UMQ (Logic/Sub) first. For Logic mode this returns the
    // borrowed jetty pair to the pool.
    for (uint32_t i = 0; i < ctx->client_cnt; i++) {
        if (ctx->sub_umqs[i].valid) {
            umq_unbind(ctx->sub_umqs[i].umqh);
            umq_destroy(ctx->sub_umqs[i].umqh);
            free(ctx->sub_umqs[i].umq_ctx);
            close(ctx->sub_umqs[i].client_fd);
        }
    }
    // Step 2: destroy donor Sub UMQs in reverse order (Logic mode only). Order doesn't matter
    // strictly, but doing it after the Logic UMQs guarantees the pool is drained first.
    if (cfg->use_logic_umq) {
        for (uint32_t i = 0; i < ctx->client_cnt; i++) {
            if (ctx->donor_umqhs[i] != UMQ_INVALID_HANDLE) {
                umq_destroy(ctx->donor_umqhs[i]);
                ctx->donor_umqhs[i] = UMQ_INVALID_HANDLE;
            }
        }
    }
    close(listen_fd);
DESTROY_MAIN:
    umq_destroy(ctx->main_umqh);
UNINIT:
    umq_uninit();
FREE_CTX:
    free(ctx);
    return 0;
}

static int latency_compare(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static uint32_t get_percentile_index(uint32_t count, double percent)
{
    uint32_t idx = (uint32_t)ceil((double)count * percent);
    return idx > count - 1 ? count - 1 : idx;
}

static void print_latency_results(uint64_t *cycles, uint32_t count, uint32_t msg_size,
                                  double test_duration_sec, double cpu_util)
{
    if (count <= LATENCY_TAIL) {
        printf("Not enough data points (%u)\n", count);
        return;
    }

    double cpu_mhz = get_cpu_mhz(false);
    double rtt_factor = 2.0;
    double cycles_to_us = cpu_mhz * rtt_factor;

    qsort(cycles, count, sizeof(uint64_t), latency_compare);
    uint32_t measure_cnt = count - LATENCY_TAIL;

    double avg_sum = 0.0;
    for (uint32_t i = 0; i < measure_cnt; i++) {
        avg_sum += (double)cycles[i] / cycles_to_us;
    }
    double avg = avg_sum / measure_cnt;

    double stdev_sum = 0.0;
    for (uint32_t i = 0; i < measure_cnt; i++) {
        double diff = avg - (double)cycles[i] / cycles_to_us;
        stdev_sum += diff * diff;
    }
    double stdev = sqrt(stdev_sum / measure_cnt);

    uint32_t idx_50 = get_percentile_index(measure_cnt, 0.5);
    uint32_t idx_99 = get_percentile_index(measure_cnt, 0.99);
    uint32_t idx_999 = get_percentile_index(measure_cnt, 0.999);
    uint32_t idx_9999 = get_percentile_index(measure_cnt, 0.9999);

    double min_lat = (double)cycles[0] / cycles_to_us;
    double max_lat = (double)cycles[measure_cnt - 1] / cycles_to_us;
    double p50_lat = (double)cycles[idx_50] / cycles_to_us;
    double p99_lat = (double)cycles[idx_99] / cycles_to_us;
    double p999_lat = (double)cycles[idx_999] / cycles_to_us;
    double p9999_lat = (double)cycles[idx_9999] / cycles_to_us;

    double bw = (double)count * msg_size / PERFTEST_1MB / test_duration_sec;
    double mpps = (double)count / test_duration_sec / PERFTEST_1M;

    printf("\n================== Client Performance Results ==================\n");
    printf("  Message size:       %u bytes\n", msg_size);
    printf("  Test iterations:    %u\n", count);
    printf("  Test duration:      %.3f s\n", test_duration_sec);
    printf("----------------------------------------------------------------\n");
    printf("  Latency Statistics (one-way, RTT/2):\n");
    printf("    Minimum:          %.2f us\n", min_lat);
    printf("    Maximum:          %.2f us\n", max_lat);
    printf("    Average:          %.2f us\n", avg);
    printf("    Median (P50):     %.2f us\n", p50_lat);
    printf("    P99:              %.2f us\n", p99_lat);
    printf("    P99.9:            %.2f us\n", p999_lat);
    printf("    P99.99:           %.2f us\n", p9999_lat);
    printf("    Stdev:            %.2f us\n", stdev);
    printf("----------------------------------------------------------------\n");
    printf("  Throughput:\n");
    printf("    Messages/sec:     %.3f Mpps\n", mpps);
    printf("    Bandwidth:        %.3f MB/s\n", bw);
    printf("----------------------------------------------------------------\n");
    printf("  CPU utilization:    %.2f%%\n", cpu_util);
    printf("================================================================\n");
}

static int run_client(m2o_config_t *cfg)
{
    client_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.force_quit = false;

    if (init_umq(cfg, false) != 0) {
        printf("init umq failed\n");
        return -1;
    }

    ctx.main_umqh = create_main_umq(cfg, false);
    if (ctx.main_umqh == UMQ_INVALID_HANDLE) {
        goto UNINIT;
    }

    umq_port_id_t port_id[2];
    port_id[0].bs.chip_id = 1;
    port_id[0].bs.die_id = 1;
    port_id[0].bs.port_idx = 4;
    port_id[1].bs.chip_id = 1;
    port_id[1].bs.die_id = 1;
    port_id[1].bs.port_idx = 5;

    umq_tp_resource_create_option_t option = { 0 };
    if (strstr(cfg->dev_name, "bond") != NULL) {
        option.create_flag = UMQ_TP_CREATE_FLAG_USED_PORTS;
        option.used_ports.port = port_id;
        option.used_ports.num = 2;
    }

    ctx.donor_umqh = ctx.main_umqh;
    if (cfg->use_logic_umq) {
        ctx.tp_handle_cnt = cfg->tp_handle_cnt;
        for (uint32_t i = 0; i < cfg->tp_handle_cnt; i++) {
            if ((ctx.tp_handle_idx[i] = umq_transport_pool_resource_create(ctx.main_umqh, &option)) > 10240) {
                printf("umq_expand_tp_resource failed\n");
                goto UNINIT;
            }
        }
    }

    umq_interrupt_fd_list_t fd_list;
    umq_interrupt_option_t fd_list_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_TX,
    };
    if (umq_interrupt_fd_list_get(ctx.main_umqh, &fd_list_option, &fd_list) != UMQ_SUCCESS) {
        printf("umq_interrupt_fd_list_get failed\n");
        goto UNINIT;
    }

    printf("umq_interrupt_fd_list_get fd cnt %u,  fd %d\n", fd_list.fd_num, fd_list.fd[0]);


    // Logic mode: create hidden donor Sub UMQ first (registers jetty pair in pool).
    // if (cfg->use_logic_umq) {
    //     ctx.donor_umqh = create_donor_sub_umq(cfg, ctx.main_umqh, 0,
    //                                           cfg->dev_name, cfg->eid_idx);
    //     if (ctx.donor_umqh == UMQ_INVALID_HANDLE) {
    //         goto DESTROY_MAIN;
    //     }
    // }

    m2o_umq_ctx_t cli_umq_ctx = {
        .sub_umqh = 0,
        .client_idx = 0,
    };
    if (cfg->use_logic_umq) {
        ctx.sub_umqh = create_logic_umq(cfg, ctx.main_umqh, (uint64_t)(uintptr_t)&cli_umq_ctx,
                                        false, cfg->dev_name, cfg->eid_idx);
    } else {
        ctx.sub_umqh = create_sub_umq(cfg, ctx.main_umqh, (uint64_t)(uintptr_t)&cli_umq_ctx,
                                      false, cfg->dev_name, cfg->eid_idx);
    }
    if (ctx.sub_umqh == UMQ_INVALID_HANDLE) {
        goto DESTROY_MAIN;
    }
    cli_umq_ctx.sub_umqh = ctx.sub_umqh;

    ctx.tcp_fd = create_tcp_client(cfg->server_ip, cfg->tcp_port);
    if (ctx.tcp_fd < 0) {
        goto DESTROY_SUB;
    }

    if (send_bind_info(ctx.tcp_fd, ctx.sub_umqh, cfg->dev_name, cfg->eid_idx) != 0) {
        printf("send bind info failed\n");
        goto CLOSE_TCP;
    }

    char srv_dev_name[64] = {0};
    uint16_t srv_eid_idx = 0;
    uint8_t srv_bind_info[UMQ_MAX_BIND_INFO_SIZE] = {0};
    uint32_t srv_bind_size = 0;
    if (recv_bind_info(ctx.tcp_fd, srv_dev_name, &srv_eid_idx, srv_bind_info, &srv_bind_size) != 0) {
        printf("recv server bind info failed\n");
        goto CLOSE_TCP;
    }

    if (umq_bind(ctx.sub_umqh, srv_bind_info, srv_bind_size) != UMQ_SUCCESS) {
        printf("bind %s umq failed\n", cfg->use_logic_umq ? "logic" : "sub");
        goto CLOSE_TCP;
    }
    printf("Client %s UMQ bound successfully\n", cfg->use_logic_umq ? "logic" : "sub");

    if (post_rx_buffers(ctx.main_umqh, cfg->buf_size, cfg->rx_depth) != 0) {
        printf("post rx buffers failed\n");
        goto UNBIND;
    }
    printf("Posted %u RX buffers to main UMQ\n", cfg->rx_depth);

    if (wait_umq_ready(ctx.sub_umqh) != 0) {
        printf("wait umq ready failed\n");
        goto UNBIND;
    }

    uint32_t sync_val;
    recv_u32(ctx.tcp_fd, &sync_val);
    printf("Sync received, starting latency test...\n");

    ctx.cycles = (uint64_t *)malloc(sizeof(uint64_t) * cfg->test_round);
    if (ctx.cycles == NULL) {
        printf("alloc cycles array failed\n");
        goto UNBIND;
    }

    uint32_t pipeline_depth = cfg->pipeline_depth;
    uint32_t tx_pool_size = pipeline_depth;

    umq_buf_t **tx_free_stack = (umq_buf_t **)malloc(sizeof(umq_buf_t *) * tx_pool_size);
    if (tx_free_stack == NULL) {
        printf("alloc tx free stack failed\n");
        goto FREE_CYCLES;
    }
    uint32_t tx_free_count = 0;

    for (uint32_t i = 0; i < tx_pool_size; i++) {
        umq_buf_t *buf = umq_buf_alloc(cfg->buf_size, 1, ctx.sub_umqh, NULL);
        if (buf == NULL) {
            printf("alloc req buf %u failed\n", i);
            for (uint32_t j = 0; j < tx_free_count; j++) {
                umq_buf_free(tx_free_stack[j]);
            }
            free(tx_free_stack);
            tx_free_stack = NULL;
            goto FREE_CYCLES;
        }
        set_pro_send_data(buf, cfg->buf_size);
        tx_free_stack[tx_free_count++] = buf;
    }

    umq_interrupt_option_t rx_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_RX,
    };
    umq_interrupt_option_t tx_option = {
        .flag = UMQ_INTERRUPT_FLAG_IO_DIRECTION,
        .direction = UMQ_IO_TX,
    };

    if (umq_rearm_interrupt(ctx.main_umqh, false, &rx_option) != UMQ_SUCCESS) {
        printf("rearm rx interrupt failed\n");
        goto FREE_REQ;
    }
    if (umq_rearm_interrupt(ctx.main_umqh, false, &tx_option) != UMQ_SUCCESS) {
        printf("rearm tx interrupt failed\n");
        goto FREE_REQ;
    }

    struct timespec cpu_start, cpu_end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start);
    uint64_t test_start_cycle = get_cycles();
    double cpu_mhz = get_cpu_mhz(true);

    uint32_t test_round = cfg->test_round;
    uint32_t outstanding = 0;
    uint32_t send_head = 0;
    uint32_t send_tail = 0;
    uint64_t *send_cycles = (uint64_t *)calloc(tx_pool_size, sizeof(uint64_t));
    if (send_cycles == NULL) {
        goto FREE_REQ;
    }

    umq_io_option_t io_tx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION | UMQ_IO_OPTION_FLAG_TP_HANDLE_IDX,
        .io_direction = UMQ_IO_TX,
        .tp_handle_idx = ctx.tp_handle_idx[0],
    };

    umq_io_option_t io_rx_ops = {
        .flag = UMQ_IO_OPTION_FLAG_DIRECTION,
        .io_direction = UMQ_IO_RX,
        .tp_handle_idx = 0,
    };

    while (ctx.iter_count < test_round && !g_force_quit) {
        while (outstanding < pipeline_depth && ctx.iter_count + outstanding < test_round) {
            if (tx_free_count == 0) {
                umq_buf_t *tx_cqes[MAX_POLL_BATCH];
                int tx_cnt = umq_poll(ctx.donor_umqh, &io_tx_ops, tx_cqes, MAX_POLL_BATCH);
                for (int ti = 0; ti < tx_cnt; ti++) {
                    if (tx_cqes[ti]->status == UMQ_FAKE_BUF_FC_UPDATE) {
                        umq_buf_free(tx_cqes[ti]);
                    } else {
                        tx_free_stack[tx_free_count++] = tx_cqes[ti];
                    }
                }
            }
            if (tx_free_count == 0) {
                break;
            }

            umq_buf_t *req_buf = tx_free_stack[--tx_free_count];
            uint64_t sc = get_cycles();
            umq_buf_t *bad_buf = NULL;
            int ret = umq_post(ctx.sub_umqh, req_buf, &io_tx_ops, &bad_buf);
            if (ret != UMQ_SUCCESS) {
                if (bad_buf != NULL && bad_buf != req_buf) {
                    umq_buf_split(req_buf, bad_buf);
                    umq_buf_free(bad_buf);
                }
                tx_free_stack[tx_free_count++] = req_buf;
                if (ret == -UMQ_ERR_EAGAIN) {
                    break;
                }

                if (ret == -UMQ_ERR_EMLINK || ret == -UMQ_ERR_ETOOMANYREFS || ret == -UMQ_ERR_ENOBUFS) {
                    usleep(10);
                    continue;
                }

                printf("post tx failed, ret %d, UMQ_ERR_EMLINK %u, UMQ_ERR_ETOOMANYREFS %u, UMQ_ERR_ENOBUFS %u\n",
                    ret, UMQ_ERR_EMLINK, UMQ_ERR_ETOOMANYREFS, UMQ_ERR_ENOBUFS);
                g_force_quit = true;
                break;
            }

            send_cycles[send_tail] = sc;
            send_tail = (send_tail + 1) % tx_pool_size;
            outstanding++;
        }

        if (outstanding == 0 && !g_force_quit) {
            if (umq_wait_interrupt(ctx.main_umqh, INTERRUPT_WAIT_MS, &rx_option) < 1) {
                umq_rearm_interrupt(ctx.main_umqh, false, &rx_option);
                continue;
            }
            umq_buf_t *fc_bufs[MAX_POLL_BATCH];
            int fc_cnt = umq_poll(ctx.main_umqh, &io_rx_ops, fc_bufs, MAX_POLL_BATCH);
            if (fc_cnt <= 0) {
                umq_rearm_interrupt(ctx.main_umqh, false, &rx_option);
                continue;
            }
            for (int fi = 0; fi < fc_cnt; fi++) {
                umq_buf_t *fb = fc_bufs[fi];
                if (fb->status == UMQ_FAKE_BUF_FC_MSG) {
                    umq_buf_free(fb);
                    umq_buf_t *sub_fc = NULL;
                    umq_poll(ctx.sub_umqh, &io_rx_ops, &sub_fc, 1);
                    if (sub_fc) {
                        umq_buf_free(sub_fc);
                    }
                    continue;
                }
                umq_buf_free(fb);
            }
            // if (fc_cnt > 0) {
            //     umq_buf_t *new_buf = umq_buf_alloc(cfg->buf_size, fc_cnt, ctx.main_umqh, NULL);
            //     if (new_buf) {
            //         umq_buf_t *bad_buf = NULL;
            //         if (umq_post(ctx.main_umqh, new_buf, UMQ_IO_RX, &bad_buf) != UMQ_SUCCESS) {
            //             umq_buf_free(bad_buf);
            //         }
            //     }
            // }
            umq_rearm_interrupt(ctx.main_umqh, false, &rx_option);
            continue;
        }

        if (umq_wait_interrupt(ctx.main_umqh, INTERRUPT_WAIT_MS, &rx_option) < 1) {
            umq_rearm_interrupt(ctx.main_umqh, false, &rx_option);
            continue;
        }

        umq_buf_t *rx_bufs[MAX_POLL_BATCH];
        int rx_cnt = umq_poll(ctx.main_umqh, &io_rx_ops, rx_bufs, MAX_POLL_BATCH);
        if (rx_cnt <= 0) {
            umq_rearm_interrupt(ctx.main_umqh, false, &rx_option);
            continue;
        }

        int fc_buf_cnt = 0;
        for (int i = 0; i < rx_cnt; i++) {
            umq_buf_t *rx_buf = rx_bufs[i];
            if (rx_buf->status == UMQ_FAKE_BUF_FC_MSG) {
                fc_buf_cnt++;
                umq_buf_free(rx_buf);
                umq_buf_t *fc_buf = NULL;
                int fc_ret = umq_poll(ctx.sub_umqh, &io_rx_ops, &fc_buf, 1);
                if (fc_ret > 0 && fc_buf) {
                    umq_buf_free(fc_buf);
                }
                continue;
            }
            if (rx_buf->status == UMQ_FAKE_BUF_FC_UPDATE) {
                fc_buf_cnt++;
                umq_buf_free(rx_buf);
                continue;
            }

            if (rx_buf->status != 0) {
                printf("============poll rx cr err %u==========\n", rx_buf->status);
                continue;
            }

            uint64_t end_cycle = get_cycles();
            if (ctx.iter_count < test_round && outstanding > 0) {
                ctx.cycles[ctx.iter_count++] = end_cycle - send_cycles[send_head];
                send_head = (send_head + 1) % tx_pool_size;
                outstanding--;
            } else if (outstanding > 0) {
                send_head = (send_head + 1) % tx_pool_size;
                outstanding--;
            }

            umq_buf_free(rx_buf);
        }

        if (rx_cnt - fc_buf_cnt > 0) {
            umq_buf_t *tx_cqes[MAX_POLL_BATCH];
            int tx_cnt = umq_poll(ctx.donor_umqh, &io_tx_ops, tx_cqes, MAX_POLL_BATCH);
            for (int ti = 0; ti < tx_cnt; ti++) {
                if (tx_cqes[ti]->status == UMQ_FAKE_BUF_FC_UPDATE) {
                    umq_buf_free(tx_cqes[ti]);
                } else {
                    tx_free_stack[tx_free_count++] = tx_cqes[ti];
                }
            }

            // printf("============tx_free_count %u, outstanding %u==========\n", tx_free_count, outstanding);

            umq_buf_t *new_buf = umq_buf_alloc(cfg->buf_size, rx_cnt - fc_buf_cnt, ctx.main_umqh, NULL);
            if (new_buf) {
                umq_buf_t *bad_buf = NULL;
                if (umq_post(ctx.main_umqh, new_buf, &io_rx_ops, &bad_buf) != UMQ_SUCCESS) {
                    umq_buf_free(bad_buf);
                }
            }
        }

        if (umq_rearm_interrupt(ctx.main_umqh, false, &rx_option) != UMQ_SUCCESS) {
            printf("rearm rx interrupt failed\n");
            break;
        }
    }

    free(send_cycles);

    uint64_t test_end_cycle = get_cycles();
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end);

    double test_duration_us = (double)(test_end_cycle - test_start_cycle) / cpu_mhz;
    double test_duration_sec = test_duration_us / PERFTEST_1M;

    double cpu_time_us = (cpu_end.tv_sec - cpu_start.tv_sec) * (double)PERFTEST_1M +
                         (cpu_end.tv_nsec - cpu_start.tv_nsec) / 1000.0;
    double cpu_util = cpu_time_us * PERCENT_NUM / test_duration_us;

    print_latency_results(ctx.cycles, ctx.iter_count, cfg->buf_size, test_duration_sec, cpu_util);

FREE_REQ:
    if (tx_free_stack) {
        for (uint32_t i = 0; i < tx_free_count; i++) {
            umq_buf_free(tx_free_stack[i]);
        }
        free(tx_free_stack);
    }
FREE_CYCLES:
    free(ctx.cycles);
UNBIND:
    umq_unbind(ctx.sub_umqh);
CLOSE_TCP:
    close(ctx.tcp_fd);
DESTROY_SUB:
    // Step 1: destroy per-client UMQ (Logic/Sub) first. For Logic mode this returns the
    // borrowed jetty pair to the pool.
    umq_destroy(ctx.sub_umqh);
DESTROY_MAIN:
    umq_destroy(ctx.main_umqh);
UNINIT:
    umq_uninit();
    return 0;
}

int main(int argc, char **argv)
{
    init_signal();

    m2o_config_t cfg;
    if (parse_arguments(argc, argv, &cfg) != 0) {
        return -1;
    }

    printf("========================================\n");
    printf("UMQ Multi-to-One Perftest\n");
    printf("  Mode:           %s\n", cfg.is_server ? "Server" : "Client");
    printf("  Device:         %s\n", cfg.dev_name);
    printf("  Transport:      %s (RC)\n", cfg.trans_mode == UMQ_TRANS_MODE_UB ? "UB" : "IB");
    printf("  Msg size:       %u\n", cfg.buf_size);
    printf("  RX depth:       %u\n", cfg.rx_depth);
    printf("  TX depth:       %u\n", cfg.tx_depth);
    printf("  Pipeline depth: %u\n", cfg.pipeline_depth);
    printf("  Flow control:   %s\n", cfg.enable_flow_control ? "ON" : "OFF");
    if (cfg.enable_flow_control) {
        printf("  Atomic window:  %s\n", cfg.use_atomic_window ? "ON" : "OFF");
        printf("  Initial credit: %u\n", cfg.initial_credit);
        printf("  Max credit req: %u\n", cfg.max_credits_request);
        printf("  FC timeout:     %u ms\n", cfg.fc_timeout_ms);
    }
    printf("  UMQ mode:       %s\n", cfg.use_logic_umq ? "Logic (jetty pool)" : "Sub (shared JFR)");
    printf("========================================\n");

    if (cfg.is_server) {
        return run_server(&cfg);
    } else {
        return run_client(&cfg);
    }
}
