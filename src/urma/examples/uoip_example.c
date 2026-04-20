/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025. All rights reserved.
 *
 * Description: UOIP driver example - test URMA over TCP
 * Author: Created for UMDK uoip driver
 *
 * This example demonstrates basic URMA operations using the uoip (URMA over IP)
 * provider:
 *   - JFC completion queue
 *   - JFR receive queue (TCP server)
 *   - JFS send queue (TCP client)
 *   - Jetty endpoint (bidirectional)
 *   - Memory segment registration
 *
 * To run:
 *   ./uoip_example [server|client] -i <local_ip> -p <local_port>
 *
 *   server: runs a TCP-bound JFR and waits for connections
 *   client: connects to remote JFR and sends data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "urma_api.h"

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */

#define DEFAULT_PORT    13857    /* Default TCP port for local JFR */
#define MSG_SIZE        1024     /* Test message size */
#define MEM_SIZE        (1 << 20) /* 1 MB buffer */

typedef struct {
    const char      *role;       /* "server" or "client" */
    const char      *local_ip;   /* For client: local IP */
    uint16_t        local_port;  /* For client: local JFR port */
    const char      *remote_ip;  /* For client: remote server IP */
    uint16_t        remote_port; /* For client: remote JFR port */
} config_t;

/* ------------------------------------------------------------------ */
/*  Test functions                                                     */
/* ------------------------------------------------------------------ */

/* Test 1: Basic send/recv using JFS and JFR */
static int test_basic_send_recv(urma_context_t *ctx)
{
    printf("=== Test 1: Basic Send/Recv ===\n");

    /* Create JFC */
    urma_jfce_t *jfce = urma_create_jfce(ctx);
    if (!jfce) {
        fprintf(stderr, "Failed to create JFCE\n");
        return -1;
    }

    urma_jfc_cfg_t jfc_cfg = {
        .depth = 64,
        .jfce = jfce,
        .user_ctx = (uint64_t)(uintptr_t)NULL,
    };

    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);
    if (!jfc) {
        fprintf(stderr, "Failed to create JFC\n");
        urma_delete_jfce(jfce);
        return -1;
    }

    /* Create JFR */
    urma_jfr_cfg_t jfr_cfg = {
        .depth = 256,
        .trans_mode = URMA_TM_RC,
        .jfc = jfc,
    };

    urma_jfr_t *jfr = urma_create_jfr(ctx, &jfr_cfg);
    if (!jfr) {
        fprintf(stderr, "Failed to create JFR\n");
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("JFC id: %u, JFR id: %u\n",
           jfc->jfc_id.id, jfr->jfr_id.id);

    /* Create JFS */
    urma_jfs_cfg_t jfs_cfg = {
        .depth = 256,
        .trans_mode = URMA_TM_RC,
        .jfc = jfc,
        .max_sge = 1,
    };

    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);
    if (!jfs) {
        fprintf(stderr, "Failed to create JFS\n");
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("JFS id: %u\n", jfs->jfs_id.id);

    /* Register segment */
    urma_seg_cfg_t seg_cfg = {
        .va = 0,
        .len = MEM_SIZE,
        .flag = {
            .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE,
            .bs.token_policy = URMA_TOKEN_NONE,
        },
    };

    urma_target_seg_t *tseg = urma_register_seg(ctx, &seg_cfg);
    if (!tseg) {
        fprintf(stderr, "Failed to register segment\n");
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Segment registered: token_id=%u va=0x%lx len=%lu\n",
           tseg->seg.token_id, tseg->seg.ubva.va, (unsigned long)tseg->seg.len);

    /* Cleanup */
    urma_unregister_seg(tseg);
    urma_delete_jfs(jfs);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);

    return 0;
}

/* Test 2: Jetty send/recv */
static int test_jetty_send_recv(urma_context_t *ctx,
                               const char *remote_ip,
                               uint16_t remote_port)
{
    printf("=== Test 2: Jetty Send/Recv ===\n");
    printf("Remote: %s:%u\n", remote_ip, remote_port);

    /* Create JFC and JFCE for completion handling */
    urma_jfce_t *jfce = urma_create_jfce(ctx);
    if (!jfce) {
        fprintf(stderr, "Failed to create JFCE\n");
        return -1;
    }

    urma_jfc_cfg_t jfc_cfg = {
        .depth = 256,
        .jfce = jfce,
        .user_ctx = (uint64_t)(uintptr_t)NULL,
    };

    urma_jfc_t *jfc = urma_create_jfc(ctx, &jfc_cfg);
    if (!jfc) {
        fprintf(stderr, "Failed to create JFC\n");
        urma_delete_jfce(jfce);
        return -1;
    }

    /* Create local Jetty with shared JFR/JFS */
    urma_jfr_cfg_t jfr_cfg = {
        .depth = 256,
        .trans_mode = URMA_TM_RC,
        .jfc = jfc,
    };
    urma_jfr_t *jfr = urma_create_jfr(ctx, &jfr_cfg);
    if (!jfr) {
        fprintf(stderr, "Failed to create JFR\n");
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    urma_jfs_cfg_t jfs_cfg = {
        .depth = 256,
        .trans_mode = URMA_TM_RC,
        .jfc = jfc,
        .max_sge = 1,
    };
    urma_jfs_t *jfs = urma_create_jfs(ctx, &jfs_cfg);
    if (!jfs) {
        fprintf(stderr, "Failed to create JFS\n");
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    urma_jetty_cfg_t jetty_cfg = {
        .flag.bs.share_jfr = 1,
        .jfs_cfg = jfs_cfg,
        .shared.jfr = jfr
    };

    urma_jetty_t *jetty = urma_create_jetty(ctx, &jetty_cfg);
    if (!jetty) {
        fprintf(stderr, "Failed to create Jetty\n");
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Local Jetty id: %u\n", jetty->jetty_id.id);

    /* Import remote Jetty */
    struct sockaddr_in remote_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(remote_port),
    };

    urma_eid_t remote_eid;
    if (inet_pton(AF_INET, remote_ip, &remote_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid remote IP: %s\n", remote_ip);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    /* Map IPv4 to IPv6-mapped URMA EID */
    remote_eid.in4.prefix = htonl(0x0000ffffu);
    remote_eid.in4.addr = remote_addr.sin_addr.s_addr;

    urma_jetty_id_t remote_jetty_id = {
        .eid = remote_eid,
        .uasid = 0,
        .id = remote_port
    };

    urma_rjetty_t rjetty = {
        .jetty_id = remote_jetty_id,
        .trans_mode = URMA_TM_RC,
        .type = URMA_JETTY,
        .flag = { .value = 0 },
        .tp_type = URMA_RTP,
    };

    urma_token_t token = { .token = 0 };
    urma_target_jetty_t *tjetty = urma_import_jetty(ctx, &rjetty, &token);
    if (!tjetty) {
        fprintf(stderr, "Failed to import Jetty\n");
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Imported remote Jetty: eid="EID_FMT", id=%u\n",
           EID_ARGS(tjetty->id.eid), tjetty->id.id);

    /* Register segment */
    urma_seg_cfg_t seg_cfg = {
        .va = 0,
        .len = MEM_SIZE,
        .flag = {
            .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE,
            .bs.token_policy = URMA_TOKEN_NONE,
        },
    };

    urma_target_seg_t *tseg = urma_register_seg(ctx, &seg_cfg);
    if (!tseg) {
        fprintf(stderr, "Failed to register segment\n");
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Segment registered: token_id=%u va=0x%lx len=%lu\n",
           tseg->seg.token_id, tseg->seg.ubva.va, (unsigned long)tseg->seg.len);

    /* Bind jetty (required for RC mode) */
    if (urma_bind_jetty(jetty, tjetty) != URMA_SUCCESS) {
        fprintf(stderr, "Failed to bind Jetty\n");
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Jetty bound successfully\n");

    /* Prepare data buffer */
    uint8_t *data = malloc(MSG_SIZE);
    if (!data) {
        fprintf(stderr, "Failed to allocate data buffer\n");
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    snprintf((char *)data, MSG_SIZE, "Hello from UOIP driver!");

    /* Post send WR */
    urma_sge_t sge = {
        .addr = (uint64_t)data,
        .len = MSG_SIZE,
        .tseg = tseg,
    };

    urma_sg_t sg = {
        .sge = &sge,
        .num_sge = 1,
    };

    urma_send_wr_t send_wr = {
        .src = sg,
        .imm_data = MSG_SIZE,
    };

    urma_jfs_wr_t wr = {
        .opcode = URMA_OPC_SEND,
        .flag.bs.complete_enable = 1,
        .send = send_wr,
        .tjetty = tjetty,
        .user_ctx = 0,
        .next = NULL,
    };

    urma_jfs_wr_t *bad_wr = NULL;
    if (urma_post_jetty_send_wr(jetty, &wr, &bad_wr) != URMA_SUCCESS) {
        fprintf(stderr, "Failed to post send WR\n");
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Posted send WR, msg=\"%s\", len=%u\n", data, MSG_SIZE);

    /* Wait for completion */
    urma_cr_t cr = {0};
    urma_jfc_t *comp_jfc = NULL;
    int ncomp = urma_wait_jfc(jfce, 1, 500, &comp_jfc);
    if (ncomp <= 0 || comp_jfc != jfc) {
        fprintf(stderr, "Timeout waiting for send completion\n");
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    ncomp = urma_poll_jfc(jfc, 1, &cr);
    if (ncomp <= 0 || cr.status != URMA_CR_SUCCESS) {
        fprintf(stderr, "Failed to poll completion, status=%d\n", cr.status);
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Send completed successfully\n");

    /* Post recv WR */
    memset(data, 0, MSG_SIZE);
    sge.addr = (uint64_t)data;

    urma_jfr_wr_t recv_wr = {
        .src = sg,
        .user_ctx = 1,
        .next = NULL,
    };

    urma_jfr_wr_t *bad_recv_wr = NULL;
    if (urma_post_jetty_recv_wr(jetty, &recv_wr, &bad_recv_wr) != URMA_SUCCESS) {
        fprintf(stderr, "Failed to post recv WR\n");
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Posted recv WR\n");

    /* Wait for recv completion */
    ncomp = urma_wait_jfc(jfce, 1, 1000, &comp_jfc);
    if (ncomp <= 0 || comp_jfc != jfc) {
        fprintf(stderr, "Timeout waiting for recv completion\n");
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    ncomp = urma_poll_jfc(jfc, 1, &cr);
    if (ncomp <= 0 || cr.status != URMA_CR_SUCCESS) {
        fprintf(stderr, "Failed to poll recv completion, status=%d\n", cr.status);
        free(data);
        urma_unbind_jetty(jetty);
        urma_unregister_seg(tseg);
        urma_unimport_jetty(tjetty);
        urma_delete_jetty(jetty);
        urma_delete_jfs(jfs);
        urma_delete_jfr(jfr);
        urma_delete_jfc(jfc);
        urma_delete_jfce(jfce);
        return -1;
    }

    printf("Recv completed, len=%lu, msg=\"%s\"\n",
           (unsigned long)cr.imm_data, (char *)data);

    /* Cleanup */
    free(data);
    urma_unbind_jetty(jetty);
    urma_unregister_seg(tseg);
    urma_unimport_jetty(tjetty);
    urma_delete_jetty(jetty);
    urma_delete_jfs(jfs);
    urma_delete_jfr(jfr);
    urma_delete_jfc(jfc);
    urma_delete_jfce(jfce);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main: parse args and run tests                                */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    printf("Usage: %s <server|client> [options]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -i <ip>       Local IP address (default: auto-detect)\n");
    printf("  -p <port>     Local JFR TCP port (default: %u)\n", DEFAULT_PORT);
    printf("  -r <ip>       Remote server IP (client mode)\n");
    printf("  -R <port>     Remote server TCP port (client mode)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s server -i 10.0.0.2 -p 13857\n", prog);
    printf("  %s client -r 10.0.0.3 -R 13857\n", prog);
    printf("\n");
}

int main(int argc, char *argv[])
{
    config_t cfg = {
        .role        = NULL,
        .local_ip    = NULL,
        .local_port  = DEFAULT_PORT,
        .remote_ip   = NULL,
        .remote_port = DEFAULT_PORT,
    };

    int opt;
    while ((opt = getopt(argc, argv, "i:p:r:R:")) != -1) {
        switch (opt) {
        case 'i':
            cfg.local_ip = optarg;
            break;
        case 'p':
            cfg.local_port = (uint16_t)atoi(optarg);
            break;
        case 'r':
            cfg.remote_ip = optarg;
            break;
        case 'R':
            cfg.remote_port = (uint16_t)atoi(optarg);
            break;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        cfg.role = argv[optind];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    if (!cfg.role || (strcmp(cfg.role, "server") != 0 &&
                        strcmp(cfg.role, "client") != 0)) {
        print_usage(argv[0]);
        return 1;
    }

    printf("UOIP driver example\n");
    printf("=====================\n");
    printf("Role: %s\n", cfg.role);
    printf("Local: %s:%u\n",
           cfg.local_ip ? cfg.local_ip : "auto", cfg.local_port);
    if (strcmp(cfg.role, "client") == 0) {
        printf("Remote: %s:%u\n", cfg.remote_ip, cfg.remote_port);
    }
    printf("\n");

    /* Initialize URMA */
    urma_init_attr_t init_attr = { .uasid = 0 };
    if (urma_init(&init_attr) != URMA_SUCCESS) {
        fprintf(stderr, "Failed to initialize URMA\n");
        return 1;
    }

    /* Get uoip device */
    urma_device_t **dev_list = NULL;
    int num_dev = 0;
    dev_list = urma_get_device_list(&num_dev);
    if (!dev_list || num_dev == 0) {
        fprintf(stderr, "No URMA devices found\n");
        urma_uninit();
        return 1;
    }

    /* Find uoip device (named "uoip0") */
    urma_device_t *dev = NULL;
    for (int i = 0; i < num_dev; i++) {
        if (strcmp(dev_list[i]->name, "uoip0") == 0) {
            dev = dev_list[i];
            break;
        }
    }

    if (!dev) {
        fprintf(stderr, "uoip device not found\n");
        urma_free_device_list(dev_list);
        urma_uninit();
        return 1;
    }

    printf("Found device: %s\n", dev->name);

    /* Create context */
    urma_context_t *ctx = urma_create_context(dev, 0);
    if (!ctx) {
        fprintf(stderr, "Failed to create URMA context\n");
        urma_free_device_list(dev_list);
        urma_uninit();
        return 1;
    }

    printf("Context created, EID: "EID_FMT"\n", EID_ARGS(ctx->eid));

    int ret = 0;
    if (strcmp(cfg.role, "server") == 0) {
        /* Server mode: just create a JFR (which binds TCP socket) */
        printf("\nRunning in server mode (JFR will listen on TCP port %u)\n",
               cfg.local_port);
        ret = test_basic_send_recv(ctx);
    } else {
        /* Client mode: connect to remote and send/recv */
        ret = test_jetty_send_recv(ctx, cfg.remote_ip, cfg.remote_port);
    }

    urma_delete_context(ctx);
    urma_free_device_list(dev_list);
    urma_uninit();

    printf("\nDone (ret=%d)\n", ret);
    return ret;
}
