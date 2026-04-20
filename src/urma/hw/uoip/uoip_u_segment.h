/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — memory segment interface
 */

#ifndef UOIP_U_SEGMENT_H
#define UOIP_U_SEGMENT_H

#include "urma_provider.h"

urma_target_seg_t *uoip_u_register_seg(urma_context_t *ctx,
                                        urma_seg_cfg_t *cfg);
urma_status_t uoip_u_unregister_seg(urma_target_seg_t *tseg);

#endif /* UOIP_U_SEGMENT_H */
