/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Bond public API header file
 * Create: 2026-08-27
 * Note:
 * History: 2026-08-27   Create File
 */
#ifndef BONDP_API_H
#define BONDP_API_H

#include "urma_ubagg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BONDP_IMM_PORT_ID_SHIFT     (22)
#define BONDP_IMM_PORT_ID_MASK      (0xFFFFULL << BONDP_IMM_PORT_ID_SHIFT)
#define BONDP_RECV_PORT_UNAVAILABLE (0)

/**
 * post a request to recv data on the bond member port.
 * @param[in] jfr: the jfr created before, which is used to put command;
 * @param[in] wr: the posting request all information, including sge, flag.
 * @param[in] port_id: the bond member port which receives data, must be a member of the jfr;
 * @param[in] bad_wr: the first of failure request.
 * Return: 0 on success, other value on error
 */
urma_status_t bondp_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
                                bondp_port_id_t port_id, urma_jfr_wr_t **bad_wr);

/**
 * post a request to recv data on the bond member port.
 * @param[in] jetty: the jetty created before, which is used to put command;
 * @param[in] wr: the posting request all information, including sge, flag.
 * @param[in] port_id: the bond member port which receives data, must be a member of the jetty;
 * @param[in] bad_wr: the first of failure request.
 * Return: 0 on success, other value on error
 */
urma_status_t bondp_post_jetty_recv_wr(urma_jetty_t *jetty, urma_jfr_wr_t *wr,
                                       bondp_port_id_t port_id, urma_jfr_wr_t **bad_wr);

#ifdef __cplusplus
}
#endif
#endif // BONDP_API_H
