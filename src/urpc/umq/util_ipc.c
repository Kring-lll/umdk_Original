/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: urpc IPC base share memory.
 * Create: 2025-5-22
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "util_ipc.h"

#define SHM_MODE (0660)
#define NUM_RING_HEADERS  (2)

typedef struct shm_ring_buf_hdr {
    volatile uint32_t avail_buf_size;
} shm_ring_buf_hdr_t;

#if defined(__x86_64__)
#define MB() asm volatile("mfence" ::: "memory")
#define RMB() asm volatile("lfence" ::: "memory")
#define WMB() asm volatile("sfence" ::: "memory")
#elif defined(__aarch64__)
#define DSB(opt) asm volatile("DSB " #opt : : : "memory")
#define MB() DSB(sy)
#define RMB() DSB(ld)
#define WMB() DSB(st)
#endif

util_ipc_t *util_ipc_create(char *ipc_name, uint32_t ipc_name_len, util_ipc_option_t *opt)
{
    int ret;
    void *ptr;
    uint32_t shm_size;
    if (ipc_name_len > MAX_IPC_NAME) {
        return NULL;
    }

    util_ipc_t *ipc_h = (util_ipc_t *)calloc(1, sizeof(util_ipc_t));
    if (ipc_h == NULL) {
        return NULL;
    }
    ipc_h->shm_fd = -1;
    int shm_fd = -1;
    if (ipc_h == NULL) {
        return NULL;
    }

    (void)memcpy(ipc_h->ipc_name, ipc_name, ipc_name_len);

    if (opt->addr == NULL) {
        shm_size = opt->tx_depth * opt->tx_max_buf_size +
                   opt->rx_depth * opt->rx_max_buf_size + NUM_RING_HEADERS * (uint32_t)sizeof(shm_ring_hdr_t);

        if (opt->owner) {
            shm_fd = shm_open(ipc_name, O_CREAT | O_RDWR | O_EXCL, SHM_MODE);
            if (shm_fd == -1) {
                goto ERR_SHM_OPEN;
            }

            // set share memory size
            ret = ftruncate(shm_fd, shm_size);
            if (ret != 0) {
                goto ERR_SHM_SIZE;
            }
        } else {
            shm_fd = shm_open(ipc_name, O_RDWR, SHM_MODE);
            if (shm_fd == -1) {
                goto ERR_SHM_OPEN;
            }
        }
        ipc_h->shm_fd = shm_fd;
        ipc_h->shm_size = shm_size;
        ipc_h->owner = opt->owner;

        ptr = mmap(0, shm_size, PROT_WRITE | PROT_READ, MAP_SHARED, shm_fd, 0);
        if (ptr == MAP_FAILED) {
            goto ERR_SHM_MMAP;
        }
    } else {
        ptr = opt->addr;
    }

    /* ipc share memory layout
    * tx ring | rx ring | tx ring header | rx ring header
    */
    ipc_h->shm_tx_ring = ptr;
    ipc_h->shm_rx_ring = ipc_h->shm_tx_ring + opt->tx_depth * opt->tx_max_buf_size;
    ipc_h->shm_tx_ring_hdr = (shm_ring_hdr_t *)(ipc_h->shm_rx_ring + opt->rx_depth * opt->rx_max_buf_size);
    ipc_h->shm_rx_ring_hdr = (shm_ring_hdr_t *)(ipc_h->shm_tx_ring_hdr + sizeof(shm_ring_hdr_t));
    if (opt->owner) {
        ipc_h->shm_tx_ring_hdr->ci = 0;
        ipc_h->shm_tx_ring_hdr->pi = 0;
        atomic_init(&ipc_h->shm_tx_ring_hdr->cq_event_flag, 0);
        atomic_init(&ipc_h->shm_tx_ring_hdr->pending_events, 0);

        ipc_h->shm_rx_ring_hdr->ci = 0;
        ipc_h->shm_rx_ring_hdr->pi = 0;
        atomic_init(&ipc_h->shm_rx_ring_hdr->cq_event_flag, 0);
        atomic_init(&ipc_h->shm_rx_ring_hdr->pending_events, 0);
    }

    ipc_h->tx_max_buf_size = opt->tx_max_buf_size;
    ipc_h->tx_depth = opt->tx_depth;
    ipc_h->rx_max_buf_size = opt->rx_max_buf_size;
    ipc_h->rx_depth = opt->rx_depth;
    return ipc_h;

ERR_SHM_MMAP:
ERR_SHM_SIZE:
    if (shm_fd != -1) {
        close(shm_fd);
    }
    if (opt->owner) {
        shm_unlink(ipc_name);
    }
ERR_SHM_OPEN:

    free(ipc_h);
    return NULL;
}

void util_ipc_destroy(util_ipc_t *ipc_h)
{
    if (ipc_h->shm_fd != -1) {
        munmap(ipc_h->shm_tx_ring, ipc_h->shm_size);
        ipc_h->shm_tx_ring = NULL;
        close(ipc_h->shm_fd);
        ipc_h->shm_fd = -1;
        if (ipc_h->owner) {
            shm_unlink(ipc_h->ipc_name);
        }
    }
    free(ipc_h);
}

int util_ipc_post_tx(util_ipc_t *ipc_h, char *tx_buf, uint32_t tx_buf_size)
{
    uint32_t payload_size = ipc_h->tx_max_buf_size - (uint32_t)sizeof(shm_ring_buf_hdr_t);
    if (tx_buf_size > payload_size) {
        return -1;
    }

    shm_ring_hdr_t *tx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_tx_ring_hdr;
    if ((tx_ring_hdr->pi + 1) % ipc_h->tx_depth == tx_ring_hdr->ci) {
        return -EAGAIN;
    }

    MB();
    shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_tx_ring +
                                                              tx_ring_hdr->pi * ipc_h->tx_max_buf_size);
    char *ring_buf_p = (char *)(ring_buf_hdr + 1);
    (void)memcpy(ring_buf_p, tx_buf, tx_buf_size);
    ring_buf_hdr->avail_buf_size = tx_buf_size;

    // Ensure that we post tx_buf before we update pi
    WMB();
    tx_ring_hdr->pi = (tx_ring_hdr->pi + 1) % ipc_h->tx_depth;

    return 0;
}

int util_ipc_post_tx_batch(util_ipc_t *ipc_h, char **tx_buf, uint32_t *tx_buf_size, uint32_t tx_buf_cnt)
{
    // rest empty count
    shm_ring_hdr_t *tx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_tx_ring_hdr;
    uint32_t left_cnt = (tx_ring_hdr->ci + ipc_h->tx_depth - tx_ring_hdr->pi - 1) % ipc_h->tx_depth;
    if (left_cnt < tx_buf_cnt) {
        return -1;
    }

    MB();
    for (uint32_t i = 0; i < tx_buf_cnt; ++i) {
        shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_tx_ring +
                                            ((tx_ring_hdr->pi + i) % ipc_h->tx_depth) * ipc_h->tx_max_buf_size);
        char *ring_buf_p = (char *)(ring_buf_hdr + 1);
        (void)memcpy(ring_buf_p, tx_buf[i], tx_buf_size[i]);
        ring_buf_hdr->avail_buf_size = tx_buf_size[i];
    }

    // Ensure that we post tx_buf before we update pi
    WMB();
    tx_ring_hdr->pi = (tx_ring_hdr->pi + tx_buf_cnt) % ipc_h->tx_depth;

    return 0;
}

int util_ipc_poll_tx(util_ipc_t *ipc_h, char *tx_buf, uint32_t tx_max_buf_size, uint32_t *avail_buf_size)
{
    shm_ring_hdr_t *tx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_tx_ring_hdr;
    if (tx_ring_hdr->pi == tx_ring_hdr->ci) {
        return -1;
    }

    RMB();
    shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_tx_ring +
                                                              tx_ring_hdr->ci * ipc_h->tx_max_buf_size);
    char *ring_buf_p = (char *)(ring_buf_hdr + 1);
    if (tx_max_buf_size < ring_buf_hdr->avail_buf_size) {
        return -1;
    }
    (void)memcpy(tx_buf, ring_buf_p, ring_buf_hdr->avail_buf_size);
    *avail_buf_size = ring_buf_hdr->avail_buf_size;

    // Ensure that we poll tx_buf before we update ci
    MB();
    tx_ring_hdr->ci = (tx_ring_hdr->ci + 1) % ipc_h->tx_depth;

    return 0;
}

int util_ipc_poll_tx_batch(util_ipc_t *ipc_h, char **tx_buf, uint32_t tx_max_buf_size,
                           uint32_t *avail_buf_size, uint32_t max_cnt)
{
    shm_ring_hdr_t *tx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_tx_ring_hdr;
    if (tx_ring_hdr->pi == tx_ring_hdr->ci) {
        return 0;
    }

    uint32_t i = 0;
    uint32_t left_cnt = (tx_ring_hdr->pi + ipc_h->tx_depth - tx_ring_hdr->ci) % ipc_h->tx_depth;
    uint32_t max_cnt_ = left_cnt < max_cnt ? left_cnt : max_cnt;
    char *tx_buf_;
    RMB();
    for (; i < max_cnt_; ++i) {
        shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_tx_ring +
                                            ((tx_ring_hdr->ci + i) % ipc_h->tx_depth) * ipc_h->tx_max_buf_size);
        char *ring_buf_p = (char *)(ring_buf_hdr + 1);
        tx_buf_ = tx_buf[i];
        if (tx_max_buf_size < ring_buf_hdr->avail_buf_size) {
            return -1;
        }
        (void)memcpy(tx_buf_, ring_buf_p, ring_buf_hdr->avail_buf_size);
        avail_buf_size[i] = ring_buf_hdr->avail_buf_size;
    }

    // Ensure that we poll tx_buf before we update ci
    MB();
    tx_ring_hdr->ci = (tx_ring_hdr->ci + i) % ipc_h->tx_depth;

    return i;
}

int util_ipc_post_rx(util_ipc_t *ipc_h, char *rx_buf, uint32_t rx_buf_size)
{
    uint32_t payload_size = ipc_h->rx_max_buf_size - (uint32_t)sizeof(shm_ring_buf_hdr_t);
    if (rx_buf_size > payload_size) {
        return -1;
    }

    shm_ring_hdr_t *rx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_rx_ring_hdr;
    if ((rx_ring_hdr->pi + 1) % ipc_h->rx_depth == rx_ring_hdr->ci) {
        return -EAGAIN;
    }

    MB();
    shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_rx_ring +
                                                              rx_ring_hdr->pi * ipc_h->rx_max_buf_size);
    char *ring_buf_p = (char *)(ring_buf_hdr + 1);
    (void)memcpy(ring_buf_p, rx_buf, rx_buf_size);
    ring_buf_hdr->avail_buf_size = rx_buf_size;

    // Ensure that we poll rx_buf before we update pi
    WMB();
    rx_ring_hdr->pi = (rx_ring_hdr->pi + 1) % ipc_h->rx_depth;

    return 0;
}

int util_ipc_poll_rx(util_ipc_t *ipc_h, char *rx_buf, uint32_t rx_max_buf_size, uint32_t *avail_buf_size)
{
    shm_ring_hdr_t *rx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_rx_ring_hdr;
    if (rx_ring_hdr->pi == rx_ring_hdr->ci) {
        return -1;
    }

    RMB();
    shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_rx_ring +
                                                              rx_ring_hdr->ci * ipc_h->rx_max_buf_size);
    char *ring_buf_p =  (char *)(ring_buf_hdr + 1);
    if (rx_max_buf_size < ring_buf_hdr->avail_buf_size) {
        return -1;
    }
    (void)memcpy(rx_buf, ring_buf_p, ring_buf_hdr->avail_buf_size);
    *avail_buf_size = ring_buf_hdr->avail_buf_size;

    // Ensure that we poll rx_buf before we update ci
    MB();
    rx_ring_hdr->ci = (rx_ring_hdr->ci + 1) % ipc_h->rx_depth;

    return 0;
}

int util_ipc_poll_rx_batch(util_ipc_t *ipc_h, char **rx_buf, uint32_t rx_max_buf_size,
                           uint32_t *avail_buf_size, uint32_t max_cnt)
{
    shm_ring_hdr_t *rx_ring_hdr = (shm_ring_hdr_t *)ipc_h->shm_rx_ring_hdr;
    if (rx_ring_hdr->pi == rx_ring_hdr->ci) {
        return 0;
    }

    uint32_t i = 0;
    uint32_t left_cnt = (rx_ring_hdr->pi + ipc_h->rx_depth - rx_ring_hdr->ci) % ipc_h->rx_depth;
    uint32_t max_cnt_ = left_cnt < max_cnt ? left_cnt : max_cnt;
    char *rx_buf_ = rx_buf[0];
    RMB();

    for (; i < max_cnt_; ++i) {
        shm_ring_buf_hdr_t *ring_buf_hdr = (shm_ring_buf_hdr_t *)(ipc_h->shm_rx_ring +
                                           ((rx_ring_hdr->ci + i) % ipc_h->rx_depth) * ipc_h->rx_max_buf_size);
        char *ring_buf_p = (char *)(ring_buf_hdr + 1);
        if (rx_max_buf_size < ring_buf_hdr->avail_buf_size) {
            return -1;
        }
        rx_buf_ = rx_buf[i];
        (void)memcpy(rx_buf_, ring_buf_p, ring_buf_hdr->avail_buf_size);
        avail_buf_size[i] = ring_buf_hdr->avail_buf_size;
    }

    // Ensure that we poll rx_buf before we update ci
    MB();
    rx_ring_hdr->ci = (rx_ring_hdr->ci + i) % ipc_h->rx_depth;

    return i;
}
