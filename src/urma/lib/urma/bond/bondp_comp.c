/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: Implementation of Bonding Component
 * Author: Ma Chuan
 * Create: 2025-02-12
 * Note:
 * History: 2025-02-12  Create file
 */
#include "urma_log.h"
#include "urma_api.h"
#include "urma_cmd.h"
#include "urma_provider.h"
#include "bondp_comp.h"



void bdp_vjfce_info_table_close_fd(bondp_comp_t *bdp_comp)
{
    int ret = URMA_SUCCESS;
    struct epoll_event ev = {0};
    // 直接遍历 bondp_comp_t 中的 p_jfce 数组，关闭所有 fd
    for (int i = 0; i < bdp_comp->dev_num; i++) {
        if (bdp_comp->p_jfce[i] != NULL) {
            ret = epoll_ctl(bdp_comp->v_jfce.fd, EPOLL_CTL_DEL, bdp_comp->p_jfce[i]->fd, &ev);
            if (ret != URMA_SUCCESS) {
                URMA_LOG_WARN("non-zero return value of EPOLL_CTL_DEL, ret = %d.\n", ret);
            }
        }
    }

    close(bdp_comp->v_jfce.fd);
}

void bdp_vjfce_info_table_destroy(bondp_hash_table_t *tbl)
{
    // 由于不再使用哈希表，直接返回
    // 实际的 p_jfce 管理由 bondp_comp_t 结构中的 p_jfce 数组负责
}

int bondp_insert_p_jfce(urma_jfce_t *v_jfce, urma_jfce_t *p_jfce)
{
    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = p_jfce->fd;
    if (epoll_ctl(v_jfce->fd, EPOLL_CTL_ADD, p_jfce->fd, &ev) != 0) {
        URMA_LOG_ERR("Fail to add fd:%d to epoll fd:%d.\n", p_jfce->fd, v_jfce->fd);
        return URMA_FAIL;
    }
    // 由于不再使用哈希表，直接返回成功
    // 实际的 p_jfce 管理由 bondp_comp_t 结构中的 p_jfce 数组负责
    return 0;
}

void bondp_remove_p_jfce(urma_jfce_t *v_jfce, urma_jfce_t *p_jfce)
{
    struct epoll_event ev = {0};
    if (epoll_ctl(v_jfce->fd, EPOLL_CTL_DEL, p_jfce->fd, &ev) != 0) {
        URMA_LOG_ERR("Fail to del fd:%d to epoll fd:%d.\n", p_jfce->fd, v_jfce->fd);
    }
    // 由于不再使用哈希表，直接返回
    // 实际的 p_jfce 管理由 bondp_comp_t 结构中的 p_jfce 数组负责
}
