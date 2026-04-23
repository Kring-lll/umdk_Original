#ifndef UBAGG_NETLINK_H
#define UBAGG_NETLINK_H

#include "ubagg_ioctl.h"

int ubagg_nl_init(void);
void ubagg_nl_uninit(void);
int ubagg_nl_get_fd(void);
int ubagg_nl_send_switchback_req(const ubagg_switchback_req_t *req);
int ubagg_nl_recv_switchback_msg(ubagg_switchback_msg_t *msg);

#endif // UBAGG_NETLINK_H
