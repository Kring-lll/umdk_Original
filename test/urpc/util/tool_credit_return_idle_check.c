/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: verify idle check triggers credit return with precise timing
 * Create: 2025-6-26
 *
 * Usage:
 *   Server: tool_credit_return_idle_check --server -d <dev> --fc-timeout-ms 1000 \
 *           --initial-credit 32 --return-ratio 2 --min-reserved-credit 2
 *   Client: tool_credit_return_idle_check --client -d <dev> --remote-ip <server_ip> \
 *           --fc-timeout-ms 1000 --initial-credit 32 --return-ratio 2 --min-reserved-credit 2
 *
 * The tool measures the actual time from last IO to credit return and compares
 * it against the configured fc-timeout-ms to verify idle check precision.
 * It tests multiple idle periods of different durations to measure jitter.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "ub_get_clock.h"
#include "umq_api.h"
#include "umq_pro_api.h"
#include "umq_dfx_api.h"

#define DEV_NAME_SIZE 128
#define MAX_INFO_SIZE 8192
#define DEFAULT_LISTEN_PORT 19877
#define DEFAULT_SIZE 64
#define DEFAULT_DEPTH 512
#define DEFAULT_INITIAL_CREDIT 32
#define DEFAULT_RETURN_RATIO 2
#define DEFAULT_MIN_RESERVED_CREDIT 2
#define DEFAULT_FC_TIMEOUT_MS 1000
#define FC_STATS_STR_LEN 4096
#define SYNC_MSG_SIZE 32
#define ACCEPT_WAIT_US 10000
#define IDLE_CHECK_POLL_MAX_MS 5000
#define IDLE_TEST_ROUND 5

typedef enum { MODE_NONE, MODE_SERVER, MODE_CLIENT } run_mode_t;

typedef struct tool_config {
    char dev_name[DEV_NAME_SIZE];
    char local_ip[INET6_ADDRSTRLEN];
    char remote_ip[INET6_ADDRSTRLEN];
    uint16_t tcp_port;
    run_mode_t mode;
    uint32_t trans_mode;
    uint32_t feature;
    uint32_t size;
    uint32_t rx_depth;
    uint32_t tx_depth;
    umq_buf_mode_t buf_mode;
    umq_tp_mode_t tp_mode;
    umq_tp_type_t tp_type;
    uint16_t eid_idx;
    bool use_atomic_window;
    umq_flow_control_cfg_t fc_cfg;
    uint32_t idle_test_round;
} tool_config_t;

static volatile bool g_force_quit = false;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        g_force_quit = true;
    }
}

static uint64_t g_umqh = UMQ_INVALID_HANDLE;
static int g_fd = -1;
static int g_accept_fd = -1;

typedef struct __attribute__((packed)) exchange_info {
    uint32_t msg_len;
    uint8_t data[MAX_INFO_SIZE];
} exchange_info_t;

static int recv_data(int sock, uint8_t *buf, uint32_t len)
{
    int offset = 0;
    while (!g_force_quit) {
        int r = recv(sock, buf + offset, len - offset, MSG_NOSIGNAL);
        if (r <= 0) {
            return -1;
        }
        offset += r;
        if (offset == (int)len) {
            break;
        }
    }
    return offset;
}

static int recv_exchange(int sock, exchange_info_t *info)
{
    return recv_data(sock, (uint8_t *)info, sizeof(exchange_info_t));
}

static int send_exchange(int sock, exchange_info_t *info)
{
    int ret = send(sock, info, sizeof(exchange_info_t), 0);
    if (ret != sizeof(exchange_info_t)) {
        return -1;
    }
    return 0;
}

static int client_sync(int fd)
{
    char msg[SYNC_MSG_SIZE] = {0};
    if (send(fd, "SYN", 3, MSG_NOSIGNAL) != 3) {
        return -1;
    }
    if (recv_data(fd, (uint8_t *)msg, 3) != 3 || memcmp(msg, "ACK", 3) != 0) {
        return -1;
    }
    return 0;
}

static int server_sync(int fd)
{
    char msg[SYNC_MSG_SIZE] = {0};
    if (recv_data(fd, (uint8_t *)msg, 3) != 3 || memcmp(msg, "SYN", 3) != 0) {
        return -1;
    }
    if (send(fd, "ACK", 3, MSG_NOSIGNAL) != 3) {
        return -1;
    }
    return 0;
}

static int create_server_socket(tool_config_t *cfg)
{
    struct sockaddr_storage addr = {0};
    socklen_t addr_len = 0;
    int fd;

    if (inet_pton(AF_INET, cfg->local_ip, &((struct sockaddr_in *)&addr)->sin_addr) == 1) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ((struct sockaddr_in *)&addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&addr)->sin_port = htons(cfg->tcp_port);
        addr_len = sizeof(struct sockaddr_in);
    } else {
        fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        ((struct sockaddr_in6 *)&addr)->sin6_family = AF_INET6;
        ((struct sockaddr_in6 *)&addr)->sin6_port = htons(cfg->tcp_port);
        inet_pton(AF_INET6, cfg->local_ip, &((struct sockaddr_in6 *)&addr)->sin6_addr);
        addr_len = sizeof(struct sockaddr_in6);
    }
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    if (bind(fd, (struct sockaddr *)&addr, addr_len) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int create_client_socket(tool_config_t *cfg)
{
    struct sockaddr_storage addr = {0};
    socklen_t addr_len = 0;
    int fd;

    if (inet_pton(AF_INET, cfg->remote_ip, &((struct sockaddr_in *)&addr)->sin_addr) == 1) {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ((struct sockaddr_in *)&addr)->sin_family = AF_INET;
        ((struct sockaddr_in *)&addr)->sin_port = htons(cfg->tcp_port);
        inet_pton(AF_INET, cfg->local_ip[0] ? cfg->local_ip : cfg->remote_ip,
                  &((struct sockaddr_in *)&addr)->sin_addr);
        addr_len = sizeof(struct sockaddr_in);
        struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(cfg->tcp_port)};
        inet_pton(AF_INET, cfg->remote_ip, &remote.sin_addr);
        if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
            close(fd);
            return -1;
        }
    } else {
        fd = -1;
    }
    return fd;
}

static int server_do_accept(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int accept_fd;
    do {
        accept_fd = accept(fd, (struct sockaddr *)&addr, &len);
        if (accept_fd >= 0) {
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
        usleep(ACCEPT_WAIT_US);
    } while (!g_force_quit);
    return accept_fd;
}

static void print_fc_stats(uint64_t umqh, const char *phase)
{
    umq_flow_control_stats_t stats = {0};
    if (umq_stats_flow_control_get(umqh, &stats) != 0) {
        printf("[%-20s] get fc stats failed\n", phase);
        return;
    }
    printf("[%-20s] pool_idle=%lu pool_be_allocated=%lu queue_acquired=%lu queue_be_allocated=%lu "
           "total_acquired=%lu total_return_send=%lu\n",
           phase,
           stats.pool_credit.pool_idle,
           stats.pool_credit.pool_be_allocated,
           stats.queue_credit.queue_acquired,
           stats.queue_credit.queue_be_allocated,
           stats.queue_credit.total_queue_acquired,
           stats.queue_credit.total_queue_post_tx_success);
}

static int process_fc_buf(umq_buf_t *buf)
{
    if (buf->status != UMQ_FAKE_BUF_FC_UPDATE) {
        return 1;
    }
    umq_buf_free(buf);
    return 0;
}

static int send_one(uint64_t umqh, umq_buf_t *buf, uint32_t size)
{
    umq_buf_t *bad_buf = NULL;
    umq_io_option_t io_tx = {.io_direction = UMQ_IO_TX};
    umq_io_option_t io_rx = {.io_direction = UMQ_IO_RX};

    umq_buf_pro_t *pro = (umq_buf_pro_t *)buf->qbuf_ext;
    pro->flag.value = 0;
    pro->flag.bs.solicited_enable = 1;
    pro->flag.bs.complete_enable = 1;
    pro->opcode = UMQ_OPC_SEND;
    if (size < 32) {
        pro->flag.bs.inline_flag = 1;
    }

    int ret = umq_post(umqh, buf, &io_tx, &bad_buf);
    if (ret == -UMQ_ERR_EAGAIN) {
        umq_buf_t *rx_buf = NULL;
        do {
            ret = umq_poll(umqh, &io_rx, &rx_buf, 1);
            if (ret < 0) {
                return -1;
            }
            if (ret == 1) {
                int fc = process_fc_buf(rx_buf);
                if (fc == 1) {
                    umq_buf_free(rx_buf);
                }
                break;
            }
        } while (!g_force_quit);
        bad_buf = NULL;
        ret = umq_post(umqh, buf, &io_tx, &bad_buf);
    }
    if (ret != UMQ_SUCCESS) {
        if (bad_buf != NULL && buf != bad_buf) {
            umq_buf_split(buf, bad_buf);
        }
        return -1;
    }

    umq_notify(umqh);

    umq_buf_t *tx_buf[UMQ_BATCH_SIZE];
    uint32_t cnt = 0;
    do {
        ret = umq_poll(umqh, &io_tx, tx_buf, 1);
        if (ret < 0) {
            return -1;
        }
        if (ret == 1) {
            int fc = process_fc_buf(tx_buf[0]);
            if (fc < 0) {
                return -1;
            }
        }
        cnt += ret;
    } while (cnt != 1 && !g_force_quit);

    return 0;
}

static int recv_one(uint64_t umqh, uint32_t size)
{
    umq_buf_t *rx_buf = NULL;
    umq_buf_t *bad_buf = NULL;
    umq_io_option_t io_rx = {.io_direction = UMQ_IO_RX};

    int ret;
    do {
        ret = umq_poll(umqh, &io_rx, &rx_buf, 1);
        if (ret < 0) {
            return -1;
        }
        if (ret == 1) {
            int fc = process_fc_buf(rx_buf);
            if (fc < 0) {
                return -1;
            }
        }
    } while (ret != 1 && !g_force_quit);

    umq_buf_free(rx_buf);
    rx_buf = umq_buf_alloc(size, 1, umqh, NULL);
    if (rx_buf == NULL) {
        return -1;
    }
    if (umq_post(umqh, rx_buf, &io_rx, &bad_buf) != UMQ_SUCCESS) {
        umq_buf_free(bad_buf);
        return -1;
    }
    return 0;
}

static int send_and_recv(uint64_t umqh, uint32_t size, uint32_t count)
{
    for (uint32_t i = 0; i < count && !g_force_quit; i++) {
        umq_buf_t *req_buf = umq_buf_alloc(size, 1, umqh, NULL);
        if (req_buf == NULL) {
            return -1;
        }
        if (send_one(umqh, req_buf, size) != 0) {
            umq_buf_free(req_buf);
            return -1;
        }
        umq_buf_free(req_buf);
        if (recv_one(umqh, size) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint64_t wait_for_credit_return(uint64_t umqh, uint64_t start_us,
    uint64_t configured_timeout_us, uint64_t *actual_return_us)
{
    umq_io_option_t io_tx = {.io_direction = UMQ_IO_TX};
    umq_io_option_t io_rx = {.io_direction = UMQ_IO_RX};
    umq_buf_t *buf = NULL;
    umq_buf_t *rx_buf = NULL;
    umq_flow_control_stats_t stats = {0};

    uint64_t acquired_before = 0;
    (void)umq_stats_flow_control_get(umqh, &stats);
    acquired_before = stats.queue_credit.queue_acquired;

    uint64_t poll_start = get_timestamp_ns() / NS_PER_US;
    uint64_t max_wait_us = configured_timeout_us + IDLE_CHECK_POLL_MAX_MS * 1000;

    while (!g_force_quit) {
        int ret = umq_poll(umqh, &io_tx, &buf, 1);
        if (ret > 0) {
            int fc = process_fc_buf(buf);
            if (fc < 0) {
                return 0;
            }
        }

        ret = umq_poll(umqh, &io_rx, &rx_buf, 1);
        if (ret > 0) {
            int fc = process_fc_buf(rx_buf);
            if (fc == 1) {
                umq_buf_free(rx_buf);
            }
        }

        (void)umq_stats_flow_control_get(umqh, &stats);
        if (stats.queue_credit.queue_acquired < acquired_before) {
            *actual_return_us = get_timestamp_ns() / NS_PER_US - start_us;
            return acquired_before - stats.queue_credit.queue_acquired;
        }

        uint64_t elapsed_us = get_timestamp_ns() / NS_PER_US - poll_start;
        if (elapsed_us > max_wait_us) {
            *actual_return_us = elapsed_us;
            return 0;
        }

        usleep(1000);
    }

    *actual_return_us = 0;
    return 0;
}

static void run_client(tool_config_t *cfg)
{
    printf("\n========================================\n");
    printf("  Credit Return Idle Check Verification (Client)\n");
    printf("========================================\n\n");
    printf("Config:\n");
    printf("  fc_timeout_ms     = %u\n", cfg->fc_cfg.timeout_ms);
    printf("  initial_credit    = %u\n", cfg->fc_cfg.initial_credit);
    printf("  return_ratio      = %u\n", cfg->fc_cfg.return_ratio);
    printf("  min_reserved_credit = %u\n", cfg->fc_cfg.min_reserved_credit);
    printf("  use_atomic_window = %d\n", cfg->use_atomic_window);
    printf("  idle_test_round   = %u\n", cfg->idle_test_round);
    printf("\n");

    uint64_t configured_timeout_us = (uint64_t)cfg->fc_cfg.timeout_ms * US_PER_MS;

    umq_init_cfg_t *umq_cfg = calloc(1, sizeof(*umq_cfg));
    if (umq_cfg == NULL) {
        printf("calloc failed\n");
        return;
    }
    umq_cfg->buf_mode = cfg->buf_mode;
    umq_cfg->feature = cfg->feature | UMQ_FEATURE_ENABLE_FLOW_CONTROL | UMQ_FEATURE_API_PRO;
    umq_cfg->flow_control = cfg->fc_cfg;
    umq_cfg->flow_control.use_atomic_window = cfg->use_atomic_window;
    umq_cfg->io_lock_free = true;
    umq_cfg->trans_info_num = 1;
    umq_cfg->trans_info[0].trans_mode = (umq_trans_mode_t)cfg->trans_mode;
    umq_cfg->headroom_size = 0;
    umq_cfg->cna = 0;

    umq_dev_assign_t *dev = &umq_cfg->trans_info[0].dev_info;
    if (strlen(cfg->dev_name) != 0) {
        dev->assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        memcpy(dev->dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        dev->dev.eid_idx = cfg->eid_idx;
    } else {
        uint32_t addr;
        if (inet_pton(AF_INET, cfg->local_ip, &addr) == 1) {
            dev->assign_mode = UMQ_DEV_ASSIGN_MODE_IPV4;
            memcpy(dev->ipv4.ip_addr, cfg->local_ip, strlen(cfg->local_ip));
        }
    }

    if (umq_init(umq_cfg) != UMQ_SUCCESS) {
        printf("umq_init failed\n");
        free(umq_cfg);
        return;
    }
    printf("umq_init success (feature=0x%x)\n", umq_cfg->feature);
    free(umq_cfg);

    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
            UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH | UMQ_CREATE_FLAG_QUEUE_MODE |
            UMQ_CREATE_FLAG_TP_MODE | UMQ_CREATE_FLAG_TP_TYPE | UMQ_CREATE_FLAG_SUB_UMQ |
            UMQ_CREATE_FLAG_SHARE_RQ | UMQ_CREATE_FLAG_MAIN_UMQ,
        .rx_buf_size = cfg->size,
        .tx_buf_size = cfg->size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_POLLING,
        .tp_mode = cfg->tp_mode,
        .tp_type = cfg->tp_type,
    };
    sprintf(option.name, "cr_idle_main");

    if (strlen(cfg->dev_name) != 0) {
        if (strstr(cfg->dev_name, "bond") != NULL) {
            option.create_flag |= UMQ_CREATE_FLAG_USED_PORTS;
            option.used_ports.port = &((umq_port_id_t){0});
            option.used_ports.num = 1;
        }
        memcpy(option.dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        option.dev_info.dev.eid_idx = cfg->eid_idx;
    }

    uint64_t main_umqh = umq_create(&option);
    if (main_umqh == UMQ_INVALID_HANDLE) {
        printf("umq_create main failed\n");
        goto UNINIT;
    }
    printf("main umqh created: 0x%lx\n", main_umqh);

    umq_create_option_t sub_option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
            UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH | UMQ_CREATE_FLAG_QUEUE_MODE |
            UMQ_CREATE_FLAG_TP_MODE | UMQ_CREATE_FLAG_TP_TYPE | UMQ_CREATE_FLAG_SUB_UMQ |
            UMQ_CREATE_FLAG_SHARE_RQ,
        .rx_buf_size = cfg->size,
        .tx_buf_size = cfg->size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_POLLING,
        .tp_mode = cfg->tp_mode,
        .tp_type = cfg->tp_type,
        .share_rq_umqh = main_umqh,
    };
    sprintf(sub_option.name, "cr_idle_sub");

    if (strlen(cfg->dev_name) != 0) {
        memcpy(sub_option.dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        sub_option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        sub_option.dev_info.dev.eid_idx = cfg->eid_idx;
    }

    uint64_t sub_umqh = umq_create(&sub_option);
    if (sub_umqh == UMQ_INVALID_HANDLE) {
        printf("umq_create sub failed\n");
        goto DESTROY_MAIN;
    }
    printf("sub umqh created: 0x%lx\n", sub_umqh);

    g_fd = create_client_socket(cfg);
    if (g_fd < 0) {
        printf("create client socket failed\n");
        goto DESTROY_SUB;
    }

    exchange_info_t local_info = {0};
    local_info.msg_len = umq_bind_info_get(sub_umqh, local_info.data, MAX_INFO_SIZE);
    if (local_info.msg_len == 0) {
        printf("umq_bind_info_get failed\n");
        goto CLOSE_SOC;
    }
    if (send_exchange(g_fd, &local_info) < 0) {
        printf("send bind info failed\n");
        goto CLOSE_SOC;
    }

    exchange_info_t remote_info = {0};
    if (recv_exchange(g_fd, &remote_info) != 0) {
        printf("recv bind info failed\n");
        goto CLOSE_SOC;
    }
    if (umq_bind(sub_umqh, remote_info.data, remote_info.msg_len) != 0) {
        printf("umq_bind sub failed\n");
        goto CLOSE_SOC;
    }
    printf("sub umq bind success\n");

    umq_io_option_t io_rx = {.io_direction = UMQ_IO_RX};
    uint32_t require = cfg->rx_depth;
    while (require > 0) {
        uint32_t batch = require > UMQ_BATCH_SIZE ? UMQ_BATCH_SIZE : require;
        umq_buf_t *buf = umq_buf_alloc(cfg->size, batch, sub_umqh, NULL);
        if (buf == NULL) {
            printf("alloc rx buf failed\n");
            goto UNBIND;
        }
        umq_buf_t *bad_buf = NULL;
        if (umq_post(sub_umqh, buf, &io_rx, &bad_buf) != UMQ_SUCCESS) {
            umq_buf_free(bad_buf);
            printf("post rx failed\n");
            goto UNBIND;
        }
        require -= batch;
    }

    umq_io_option_t io_tx_wait = {.io_direction = UMQ_IO_TX};
    umq_buf_t *wait_buf = NULL;
    int wait_cnt = 0;
    do {
        umq_poll(sub_umqh, &io_tx_wait, &wait_buf, 1);
        umq_state_t state = umq_state_get(sub_umqh);
        if (state != QUEUE_STATE_IDLE) {
            break;
        }
        usleep(100);
    } while (wait_cnt++ < 10000);

    if (umq_state_get(sub_umqh) != QUEUE_STATE_READY) {
        printf("wait queue ready failed, state %d\n", umq_state_get(sub_umqh));
        goto UNBIND;
    }
    printf("sub umq ready\n");

    if (client_sync(g_fd) != 0) {
        printf("sync failed\n");
        goto UNBIND;
    }

    print_fc_stats(sub_umqh, "initial");

    printf("\n--- Phase 1: Acquire credits by sending %u messages ---\n",
           cfg->fc_cfg.initial_credit > 0 ? cfg->fc_cfg.initial_credit : 8);
    uint32_t acquire_count = cfg->fc_cfg.initial_credit > 0 ? cfg->fc_cfg.initial_credit : 8;
    if (send_and_recv(sub_umqh, cfg->size, acquire_count) != 0) {
        printf("send_and_recv for acquire failed\n");
        goto UNBIND;
    }
    print_fc_stats(sub_umqh, "after-acquire");

    printf("\n--- Phase 2: Idle check timing verification (%u rounds) ---\n\n",
           cfg->idle_test_round);

    uint64_t min_delta_us = UINT64_MAX;
    uint64_t max_delta_us = 0;
    uint64_t sum_delta_us = 0;
    uint32_t pass_count = 0;
    uint32_t fail_count = 0;

    for (uint32_t round = 0; round < cfg->idle_test_round && !g_force_quit; round++) {
        printf("  Round %u/%u:\n", round + 1, cfg->idle_test_round);

        if (send_and_recv(sub_umqh, cfg->size, 1) != 0) {
            printf("    pre-send failed\n");
            break;
        }

        uint64_t last_send_ns = get_timestamp_ns();
        uint64_t last_send_us = last_send_ns / NS_PER_US;

        printf("    last_send_ts = %lu ns, going idle...\n", last_send_ns);

        usleep((configured_timeout_us / 2) * 1000);

        umq_io_option_t io_tx_poll = {.io_direction = UMQ_IO_TX};
        umq_buf_t *dummy;
        umq_poll(sub_umqh, &io_tx_poll, &dummy, 1);

        uint64_t actual_return_us = 0;
        uint64_t returned_credits = wait_for_credit_return(sub_umqh, last_send_us,
            configured_timeout_us, &actual_return_us);

        uint64_t delta_us = actual_return_us - configured_timeout_us;
        double delta_ms = (double)delta_us / US_PER_MS;
        double actual_ms = (double)actual_return_us / US_PER_MS;
        double configured_ms = (double)cfg->fc_cfg.timeout_ms;

        if (returned_credits > 0) {
            pass_count++;
            printf("    CREDIT RETURNED: %lu credits returned\n", returned_credits);
            printf("    configured_timeout = %.2f ms, actual_return_time = %.2f ms, "
                   "delta = %.2f ms (%.1f%%)\n",
                   configured_ms, actual_ms, delta_ms,
                   delta_ms * 100.0 / configured_ms);

            if (delta_us < min_delta_us) {
                min_delta_us = delta_us;
            }
            if (delta_us > max_delta_us) {
                max_delta_us = delta_us;
            }
            sum_delta_us += delta_us;
        } else {
            fail_count++;
            printf("    NO CREDIT RETURN detected after %.2f ms (max wait exceeded)\n",
                   actual_ms);
        }

        print_fc_stats(sub_umqh, round < cfg->idle_test_round - 1 ? "  after-round" : "after-all-rounds");
    }

    printf("\n========================================\n");
    printf("  Idle Check Timing Verification Result\n");
    printf("========================================\n\n");
    printf("  Total rounds:       %u\n", cfg->idle_test_round);
    printf("  Credit returned:    %u (PASS)\n", pass_count);
    printf("  No return detected: %u (FAIL)\n", fail_count);
    printf("  Configured timeout: %u ms\n", cfg->fc_cfg.timeout_ms);

    if (pass_count > 0) {
        double avg_ms = (double)(sum_delta_us / pass_count) / US_PER_MS;
        double min_ms = (double)min_delta_us / US_PER_MS;
        double max_ms = (double)max_delta_us / US_PER_MS;
        printf("  Min delta:          %.2f ms\n", min_ms);
        printf("  Max delta:          %.2f ms\n", max_ms);
        printf("  Avg delta:          %.2f ms\n", avg_ms);
        printf("  Precision jitter:   %.2f ms (max - min)\n", max_ms - min_ms);

        double avg_pct = avg_ms * 100.0 / cfg->fc_cfg.timeout_ms;
        printf("  Avg deviation:      %.1f%% of configured timeout\n", avg_pct);
    }

    printf("\n--- Phase 3: Resume send after idle, verify system functional ---\n");
    if (send_and_recv(sub_umqh, cfg->size, acquire_count) != 0) {
        printf("  FAIL: cannot send after idle\n");
    } else {
        printf("  PASS: send after idle works, %u messages sent successfully\n", acquire_count);
    }

    print_fc_stats(sub_umqh, "after-resume");

    printf("\n=== Verification Complete ===\n\n");

UNBIND:
    (void)umq_unbind(sub_umqh);
DESTROY_SUB:
    (void)umq_destroy(sub_umqh);
DESTROY_MAIN:
    (void)umq_destroy(main_umqh);
CLOSE_SOC:
    if (g_fd >= 0) {
        close(g_fd);
    }
UNINIT:
    umq_uninit();
}

static void run_server(tool_config_t *cfg)
{
    printf("Server mode: waiting for connections on port %u\n", cfg->tcp_port);

    umq_init_cfg_t *umq_cfg = calloc(1, sizeof(*umq_cfg));
    if (umq_cfg == NULL) {
        return;
    }
    umq_cfg->buf_mode = cfg->buf_mode;
    umq_cfg->feature = cfg->feature | UMQ_FEATURE_ENABLE_FLOW_CONTROL | UMQ_FEATURE_API_PRO;
    umq_cfg->flow_control = cfg->fc_cfg;
    umq_cfg->flow_control.use_atomic_window = cfg->use_atomic_window;
    umq_cfg->io_lock_free = true;
    umq_cfg->trans_info_num = 1;
    umq_cfg->trans_info[0].trans_mode = (umq_trans_mode_t)cfg->trans_mode;
    umq_cfg->headroom_size = 0;
    umq_cfg->cna = 0;

    umq_dev_assign_t *dev = &umq_cfg->trans_info[0].dev_info;
    if (strlen(cfg->dev_name) != 0) {
        dev->assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        memcpy(dev->dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        dev->dev.eid_idx = cfg->eid_idx;
    } else {
        uint32_t addr;
        if (inet_pton(AF_INET, cfg->local_ip, &addr) == 1) {
            dev->assign_mode = UMQ_DEV_ASSIGN_MODE_IPV4;
            memcpy(dev->ipv4.ip_addr, cfg->local_ip, strlen(cfg->local_ip));
        }
    }

    if (umq_init(umq_cfg) != UMQ_SUCCESS) {
        printf("umq_init failed\n");
        free(umq_cfg);
        return;
    }
    free(umq_cfg);

    umq_create_option_t option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
            UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH | UMQ_CREATE_FLAG_QUEUE_MODE |
            UMQ_CREATE_FLAG_TP_MODE | UMQ_CREATE_FLAG_TP_TYPE | UMQ_CREATE_FLAG_SUB_UMQ |
            UMQ_CREATE_FLAG_SHARE_RQ | UMQ_CREATE_FLAG_MAIN_UMQ,
        .rx_buf_size = cfg->size,
        .tx_buf_size = cfg->size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_POLLING,
        .tp_mode = cfg->tp_mode,
        .tp_type = cfg->tp_type,
    };
    sprintf(option.name, "cr_idle_srv_main");

    if (strlen(cfg->dev_name) != 0) {
        memcpy(option.dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        option.dev_info.dev.eid_idx = cfg->eid_idx;
    }

    uint64_t main_umqh = umq_create(&option);
    if (main_umqh == UMQ_INVALID_HANDLE) {
        printf("umq_create main failed\n");
        goto UNINIT;
    }

    umq_create_option_t sub_option = {
        .trans_mode = (umq_trans_mode_t)cfg->trans_mode,
        .create_flag = UMQ_CREATE_FLAG_RX_BUF_SIZE | UMQ_CREATE_FLAG_TX_BUF_SIZE |
            UMQ_CREATE_FLAG_RX_DEPTH | UMQ_CREATE_FLAG_TX_DEPTH | UMQ_CREATE_FLAG_QUEUE_MODE |
            UMQ_CREATE_FLAG_TP_MODE | UMQ_CREATE_FLAG_TP_TYPE | UMQ_CREATE_FLAG_SUB_UMQ |
            UMQ_CREATE_FLAG_SHARE_RQ,
        .rx_buf_size = cfg->size,
        .tx_buf_size = cfg->size,
        .rx_depth = cfg->rx_depth,
        .tx_depth = cfg->tx_depth,
        .mode = UMQ_MODE_POLLING,
        .tp_mode = cfg->tp_mode,
        .tp_type = cfg->tp_type,
        .share_rq_umqh = main_umqh,
    };
    sprintf(sub_option.name, "cr_idle_srv_sub");

    if (strlen(cfg->dev_name) != 0) {
        memcpy(sub_option.dev_info.dev.dev_name, cfg->dev_name, strlen(cfg->dev_name));
        sub_option.dev_info.assign_mode = UMQ_DEV_ASSIGN_MODE_DEV;
        sub_option.dev_info.dev.eid_idx = cfg->eid_idx;
    }

    uint64_t sub_umqh = umq_create(&sub_option);
    if (sub_umqh == UMQ_INVALID_HANDLE) {
        printf("umq_create sub failed\n");
        goto DESTROY_MAIN;
    }

    g_fd = create_server_socket(cfg);
    if (g_fd < 0) {
        printf("create server socket failed\n");
        goto DESTROY_SUB;
    }

    g_accept_fd = server_do_accept(g_fd);
    if (g_accept_fd < 0) {
        printf("accept failed\n");
        goto CLOSE_FD;
    }

    exchange_info_t remote_info = {0};
    if (recv_exchange(g_accept_fd, &remote_info) != 0) {
        printf("recv bind info failed\n");
        goto CLOSE_ACCEPT;
    }

    exchange_info_t local_info = {0};
    local_info.msg_len = umq_bind_info_get(sub_umqh, local_info.data, MAX_INFO_SIZE);
    if (local_info.msg_len == 0) {
        printf("umq_bind_info_get failed\n");
        goto CLOSE_ACCEPT;
    }
    if (send_exchange(g_accept_fd, &local_info) < 0) {
        printf("send bind info failed\n");
        goto CLOSE_ACCEPT;
    }
    if (umq_bind(sub_umqh, remote_info.data, remote_info.msg_len) != 0) {
        printf("umq_bind failed\n");
        goto CLOSE_ACCEPT;
    }

    umq_io_option_t io_rx = {.io_direction = UMQ_IO_RX};
    uint32_t require = cfg->rx_depth;
    while (require > 0) {
        uint32_t batch = require > UMQ_BATCH_SIZE ? UMQ_BATCH_SIZE : require;
        umq_buf_t *buf = umq_buf_alloc(cfg->size, batch, sub_umqh, NULL);
        if (buf == NULL) {
            goto UNBIND;
        }
        umq_buf_t *bad_buf = NULL;
        if (umq_post(sub_umqh, buf, &io_rx, &bad_buf) != UMQ_SUCCESS) {
            umq_buf_free(bad_buf);
            goto UNBIND;
        }
        require -= batch;
    }

    umq_io_option_t io_tx_wait = {.io_direction = UMQ_IO_TX};
    umq_buf_t *wait_buf = NULL;
    int wait_cnt = 0;
    do {
        umq_poll(sub_umqh, &io_tx_wait, &wait_buf, 1);
        if (umq_state_get(sub_umqh) != QUEUE_STATE_IDLE) {
            break;
        }
        usleep(100);
    } while (wait_cnt++ < 10000);

    if (server_sync(g_accept_fd) != 0) {
        printf("sync failed\n");
        goto UNBIND;
    }

    uint64_t server_timeout_us = (uint64_t)cfg->fc_cfg.timeout_ms * 3 * US_PER_MS +
        (uint64_t)cfg->idle_test_round * (uint64_t)cfg->fc_cfg.timeout_ms * US_PER_MS +
        IDLE_CHECK_POLL_MAX_MS * 1000 * cfg->idle_test_round +
        5000000;
    uint64_t start_cycle = get_cycles();
    double mhz = get_cpu_mhz(false);

    umq_buf_t *resp_buf = umq_buf_alloc(cfg->size, 1, sub_umqh, NULL);
    if (resp_buf == NULL) {
        goto UNBIND;
    }
    umq_buf_pro_t *pro = (umq_buf_pro_t *)resp_buf->qbuf_ext;
    pro->flag.value = 0;
    pro->flag.bs.solicited_enable = 1;
    pro->flag.bs.complete_enable = 1;
    pro->opcode = UMQ_OPC_SEND;

    umq_io_option_t io_rx_opt = {.io_direction = UMQ_IO_RX};
    umq_io_option_t io_tx_opt = {.io_direction = UMQ_IO_TX};

    while (!g_force_quit && (get_cycles() - start_cycle) / mhz < server_timeout_us) {
        umq_buf_t *rx_buf = NULL;
        uint32_t recv_cnt = 0;
        do {
            int ret = umq_poll(sub_umqh, &io_rx_opt, &rx_buf, 1);
            if (ret < 0) {
                break;
            }
            if (ret == 1) {
                int fc = process_fc_buf(rx_buf);
                if (fc < 0) {
                    break;
                }
            }
            recv_cnt += ret;
        } while (recv_cnt < 1 && !g_force_quit);

        if (recv_cnt == 0) {
            continue;
        }

        umq_buf_free(rx_buf);
        rx_buf = umq_buf_alloc(cfg->size, 1, sub_umqh, NULL);
        if (rx_buf == NULL) {
            break;
        }
        umq_buf_t *bad_buf = NULL;
        if (umq_post(sub_umqh, rx_buf, &io_rx_opt, &bad_buf) != UMQ_SUCCESS) {
            umq_buf_free(bad_buf);
            break;
        }

        int ret = umq_post(sub_umqh, resp_buf, &io_tx_opt, &bad_buf);
        if (ret == -UMQ_ERR_EAGAIN) {
            umq_buf_t *fc_buf = NULL;
            do {
                ret = umq_poll(sub_umqh, &io_rx_opt, &fc_buf, 1);
                if (ret == 1) {
                    int fc = process_fc_buf(fc_buf);
                    if (fc == 1) {
                        umq_buf_free(fc_buf);
                    }
                    break;
                }
            } while (!g_force_quit);
            ret = umq_post(sub_umqh, resp_buf, &io_tx_opt, &bad_buf);
        }
        if (ret != UMQ_SUCCESS) {
            if (bad_buf != NULL && resp_buf != bad_buf) {
                umq_buf_split(resp_buf, bad_buf);
            }
            break;
        }

        umq_buf_t *tx_buf[UMQ_BATCH_SIZE];
        uint32_t tx_cnt = 0;
        do {
            ret = umq_poll(sub_umqh, &io_tx_opt, tx_buf, 1);
            if (ret < 0) {
                break;
            }
            if (ret == 1) {
                int fc = process_fc_buf(tx_buf[0]);
                if (fc < 0) {
                    break;
                }
            }
            tx_cnt += ret;
        } while (tx_cnt != 1 && !g_force_quit);
    }

    umq_buf_free(resp_buf);

UNBIND:
    (void)umq_unbind(sub_umqh);
DESTROY_SUB:
    (void)umq_destroy(sub_umqh);
DESTROY_MAIN:
    (void)umq_destroy(main_umqh);
CLOSE_ACCEPT:
    if (g_accept_fd >= 0) {
        close(g_accept_fd);
    }
CLOSE_FD:
    if (g_fd >= 0) {
        close(g_fd);
    }
UNINIT:
    umq_uninit();
}

static struct option g_long_options[] = {
    {"server",           no_argument,       NULL, 'S'},
    {"client",           no_argument,       NULL, 'C'},
    {"dev",              required_argument, NULL, 'd'},
    {"local-ip",         required_argument, NULL, 'l'},
    {"remote-ip",        required_argument, NULL, 'r'},
    {"port",             required_argument, NULL, 'p'},
    {"trans-mode",       required_argument, NULL, 'T'},
    {"size",             required_argument, NULL, 's'},
    {"rx-depth",         required_argument, NULL, 'R'},
    {"tx-depth",         required_argument, NULL, 'U'},
    {"buf-mode",         required_argument, NULL, 'b'},
    {"tp-mode",          required_argument, NULL, 'M'},
    {"tp-type",          required_argument, NULL, 'P'},
    {"eid-index",        required_argument, NULL, 'E'},
    {"use-atomic-window",no_argument,       NULL, 'A'},
    {"initial-credit",   required_argument, NULL, 'i'},
    {"max-credits-request", required_argument, NULL, 'm'},
    {"credit-multiple",  required_argument, NULL, 'g'},
    {"return-ratio",     required_argument, NULL, 'o'},
    {"min-reserved-credit", required_argument, NULL, 'v'},
    {"fc-timeout-ms",    required_argument, NULL, 'w'},
    {"idle-round",       required_argument, NULL, 'k'},
    {"help",             no_argument,       NULL, 'h'},
    {NULL, 0, NULL, 0}
};

static void usage(void)
{
    printf("Credit Return Idle Check Verification Tool\n\n");
    printf("Usage:\n");
    printf("  --server                           run as server\n");
    printf("  --client                           run as client\n");
    printf("  -d, --dev <dev>                    device name\n");
    printf("  -l, --local-ip <ip>                local IP\n");
    printf("  -r, --remote-ip <ip>               remote IP (client only)\n");
    printf("  -p, --port <port>                  TCP port (default: %u)\n", DEFAULT_LISTEN_PORT);
    printf("  -T, --trans-mode <mode>            transport mode (default: UB=2)\n");
    printf("  -s, --size <size>                  message size (default: %u)\n", DEFAULT_SIZE);
    printf("  -R, --rx-depth <depth>             rx depth (default: %u)\n", DEFAULT_DEPTH);
    printf("  -U, --tx-depth <depth>             tx depth (default: %u)\n", DEFAULT_DEPTH);
    printf("  -i, --initial-credit <num>         initial credit (default: %u)\n", DEFAULT_INITIAL_CREDIT);
    printf("  -o, --return-ratio <num>           return ratio 1=all 2=half (default: %u)\n", DEFAULT_RETURN_RATIO);
    printf("  -v, --min-reserved-credit <num>    min reserved credit (default: %u)\n", DEFAULT_MIN_RESERVED_CREDIT);
    printf("  -w, --fc-timeout-ms <ms>           idle timeout in ms (default: %u)\n", DEFAULT_FC_TIMEOUT_MS);
    printf("  -A, --use-atomic-window            use atomic window for FC\n");
    printf("  -k, --idle-round <num>             number of idle test rounds (default: %u)\n", IDLE_TEST_ROUND);
    printf("  -h, --help                         show this help\n\n");
}

static void init_cfg(tool_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tcp_port = DEFAULT_LISTEN_PORT;
    cfg->trans_mode = UMQ_TRANS_MODE_UB;
    cfg->feature = UMQ_FEATURE_API_PRO | UMQ_FEATURE_ENABLE_FLOW_CONTROL;
    cfg->size = DEFAULT_SIZE;
    cfg->rx_depth = DEFAULT_DEPTH;
    cfg->tx_depth = DEFAULT_DEPTH;
    cfg->buf_mode = UMQ_BUF_SPLIT;
    cfg->tp_mode = UMQ_TM_RC;
    cfg->tp_type = UMQ_TP_TYPE_CTP;
    cfg->eid_idx = 0;
    cfg->use_atomic_window = false;
    cfg->fc_cfg.initial_credit = DEFAULT_INITIAL_CREDIT;
    cfg->fc_cfg.return_ratio = DEFAULT_RETURN_RATIO;
    cfg->fc_cfg.min_reserved_credit = DEFAULT_MIN_RESERVED_CREDIT;
    cfg->fc_cfg.timeout_ms = DEFAULT_FC_TIMEOUT_MS;
    cfg->idle_test_round = IDLE_TEST_ROUND;
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    tool_config_t cfg = {0};
    init_cfg(&cfg);

    if (argc == 1) {
        usage();
        return -1;
    }

    while (1) {
        int c = getopt_long(argc, argv, "d:l:r:p:s:hSC:T:R:U:b:M:P:E:Ai:m:g:o:v:w:k:",
                            g_long_options, NULL);
        if (c == -1) {
            break;
        }
        switch (c) {
        case 'S': cfg.mode = MODE_SERVER; break;
        case 'C': cfg.mode = MODE_CLIENT; break;
        case 'd':
            snprintf(cfg.dev_name, DEV_NAME_SIZE, "%s", optarg);
            break;
        case 'l':
            snprintf(cfg.local_ip, sizeof(cfg.local_ip), "%s", optarg);
            break;
        case 'r':
            snprintf(cfg.remote_ip, sizeof(cfg.remote_ip), "%s", optarg);
            break;
        case 'p': cfg.tcp_port = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'T': cfg.trans_mode = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 's': cfg.size = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'R': cfg.rx_depth = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'U': cfg.tx_depth = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'b': cfg.buf_mode = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'M': cfg.tp_mode = (umq_tp_mode_t)strtoul(optarg, NULL, 0); break;
        case 'P': cfg.tp_type = (umq_tp_type_t)strtoul(optarg, NULL, 0); break;
        case 'E': cfg.eid_idx = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'A': cfg.use_atomic_window = true; break;
        case 'i': cfg.fc_cfg.initial_credit = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'm': cfg.fc_cfg.max_credits_request = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'g': cfg.fc_cfg.credit_multiple = (float)strtod(optarg, NULL); break;
        case 'o': cfg.fc_cfg.return_ratio = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'v': cfg.fc_cfg.min_reserved_credit = (uint16_t)strtoul(optarg, NULL, 0); break;
        case 'w': cfg.fc_cfg.timeout_ms = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'k': cfg.idle_test_round = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'h': usage(); return 0;
        default: usage(); return -1;
        }
    }

    if (cfg.mode == MODE_NONE) {
        printf("Must specify --server or --client\n");
        usage();
        return -1;
    }

    if (strlen(cfg.dev_name) == 0 && strlen(cfg.local_ip) == 0) {
        printf("Must specify --dev or --local-ip\n");
        return -1;
    }

    if (cfg.mode == MODE_CLIENT && strlen(cfg.remote_ip) == 0) {
        printf("Client must specify --remote-ip\n");
        return -1;
    }

    if (cfg.mode == MODE_SERVER) {
        run_server(&cfg);
    } else {
        run_client(&cfg);
    }

    return 0;
}
