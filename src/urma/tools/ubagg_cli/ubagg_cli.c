/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: tools for handling ubagg devices by command line
 * Author: Dongxu Li
 * Create: 2025-01-14
 * Note:
 * History:
 */
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include "ub_util.h"
#include "urma_api.h"
#include "ubagg_cli.h"

#define MAX_DEV_NUM                (16)
#define MAX_DEV_NAME               (64)
#define IPV4_MAP_IPV6_PREFIX       (0x0000ffff)
#define EID_STR_MIN_LEN            (3)

typedef enum ubagg_config_type {
    CONFIG_ADD_DEV = 1,
    CONFIG_RMV_DEV,
} ubagg_config_type_t;

struct ubagg_config {
    ubagg_config_type_t type;
    int slave_dev_num;
    char master_dev_name[MAX_DEV_NAME];
    char slave_dev_name[MAX_DEV_NUM][MAX_DEV_NAME];
    urma_eid_t eid;
};

static inline void ipv4_map_to_eid(uint32_t ipv4, urma_eid_t *eid)
{
    eid->in4.reserved = 0;
    eid->in4.prefix = htobe32(IPV4_MAP_IPV6_PREFIX);
    eid->in4.addr = htobe32(ipv4);
}

static int str_to_eid(const char *buf, urma_eid_t *eid)
{
    uint32_t ipv4;

    if (buf == NULL || strlen(buf) <= EID_STR_MIN_LEN || eid == NULL) {
        return -EINVAL;
    }

    // ipv4 addr: xx.xx.xx.xx
    if (inet_pton(AF_INET, buf, &ipv4) > 0) {
        ipv4_map_to_eid(be32toh(ipv4), eid);
        return 0;
    }

    // ipv6 addr
    if (inet_pton(AF_INET6, buf, eid) <= 0) {
        return -EINVAL;
    }
    return 0;
}

static void usage()
{
    (void)printf("Usage:\n");
    (void)printf("add ubagg device:    ubagg_cli -t add_dev -m MASTER_DEV -s SLAVE_DEV1 -s SLAVE_DEV2 ... -e EID\n");
    (void)printf("remvoe ubagg device: ubagg_cli -t rmv_dev -m MASTER_DEV\n");
    (void)printf(" -h, --help                   show this help info.\n");
    (void)printf(" -t, --type <type_name>       type_name can be add_dev/rmv_dev to add/remove ubagg device.\n");
    (void)printf(" -m, --master_dev <dev_name>  specify the master device name to add/remove ubagg device\n");
    (void)printf(" -s, --slave_dev <dev_name>   specify the slave device names in adding ubagg device,\n");
    (void)printf("                              if add device, specify the slave device by -s dev1, -s dev2 ...\n");
    (void)printf("                              if remove device, slave device is not needed.\n");
    (void)printf(" -e, --eid <eid>              set eid to ubagg device.\n");
}

static const struct option g_ubagg_cli_long_options[] = {
    {"help",               no_argument,        NULL, 'h'},
    {"type",               required_argument,  NULL, 't'},
    {"slave_dev",          required_argument,  NULL, 's'},
    {"master_dev",         required_argument,  NULL, 'm'},
    {"eid",                required_argument,  NULL, 'e'},
    {NULL,                 no_argument,        NULL, '\0'}
};

int parse_args(int argc, char *argv[], struct ubagg_config *config)
{
    int c;
    config->slave_dev_num = 0;
    if (argc <= 1) {
        usage();
        return -1;
    }
    while (1) {
        c = getopt_long(argc, argv, "t:s:m:e:h", g_ubagg_cli_long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 't':
                if (strcmp(optarg, "add_dev") == 0) {
                    config->type = CONFIG_ADD_DEV;
                } else if (strcmp(optarg, "rmv_dev") == 0) {
                    config->type = CONFIG_RMV_DEV;
                } else {
                    fprintf(stderr, "invalid type:%s\n", optarg);
                    return -1;
                }
                break;
            case 's':
                (void)strncpy(config->slave_dev_name[config->slave_dev_num], optarg, MAX_DEV_NAME);
                config->slave_dev_name[config->slave_dev_num][MAX_DEV_NAME - 1] = '\0';
                config->slave_dev_num++;
                break;
            case 'm':
                (void)strncpy(config->master_dev_name, optarg, MAX_DEV_NAME);
                config->master_dev_name[MAX_DEV_NAME - 1] = '\0';
                break;
            case 'e':
                if (str_to_eid(optarg, &config->eid) != 0) {
                    fprintf(stderr, "str to eid fail,str::%s\n", optarg);
                    return -1;
                }
                break;
            case 'h':
            default:
                usage();
                return -1;
        }
    }
    if (config->type == 0) {
        fprintf(stderr, "wrong type name\n");
        usage();
        return -1;
    }
    return 0;
}

static int query_slaves_and_get_master_dev_cap(struct ubagg_config *config, urma_device_cap_t *master_dev_cap)
{
    urma_device_t *dev = NULL;
    urma_init_attr_t init_attr = {0};
    urma_device_attr_t slave_attr;
    urma_init(&init_attr);

    for (int i = 0; i < config->slave_dev_num; ++i) {
        dev = urma_get_device_by_name(config->slave_dev_name[i]);
        if (dev == NULL) {
            fprintf(stderr, "can not find device %s\n", config->slave_dev_name[i]);
            continue;
        }
        if (urma_query_device(dev, &slave_attr) != URMA_SUCCESS) {
            fprintf(stderr, "query device %s failed\n", config->slave_dev_name[i]);
            continue;
        }
        break;
    }
    *master_dev_cap = slave_attr.dev_cap;
    urma_uninit();
    return 0;
}

int ubagg_ioctl_add_dev(struct ubagg_config *config)
{
    struct ubagg_add_dev args = {0};
    struct ubagg_cmd_hdr hdr = {0};
    int dev_fd;
    int ret;

    if (config->type != CONFIG_ADD_DEV) {
        fprintf(stderr, "wrong operation type.\n");
        return -1;
    }

    if (query_slaves_and_get_master_dev_cap(config, &args.in.dev_cap)) {
        fprintf(stderr, "failed to query slave attr\n");
        return -1;
    }

    (void)strncpy(args.in.master_dev_name, config->master_dev_name, MAX_DEV_NAME);
    args.in.master_dev_name[MAX_DEV_NAME - 1] = '\0';
    args.in.slave_dev_num = config->slave_dev_num;
    for (int i = 0; i < config->slave_dev_num; i++) {
        (void)strncpy(args.in.slave_dev_name[i], config->slave_dev_name[i], MAX_DEV_NAME);
        args.in.slave_dev_name[i][MAX_DEV_NAME - 1] = '\0';
    }
    (void)memcpy(&args.in.eid, &config->eid, sizeof(config->eid));

    hdr.command = UBAGG_ADD_DEV;
    hdr.args_len = sizeof(args);
    hdr.args_addr = (uint64_t)&args;

    dev_fd = open("/dev/ubagg", O_RDWR);
    if (dev_fd == -1) {
        fprintf(stderr, "Failed to open dev_fd err: %s.\n", ub_strerror(errno));
        return -1;
    }

    ret = ioctl(dev_fd, UBAGG_CMD, &hdr);
    if (ret != 0) {
        fprintf(stderr, "ioctl to add ubagg device fail\n");
        close(dev_fd);
        return -1;
    }

    close(dev_fd);
    return 0;
}

int ubagg_ioctl_rmv_dev(struct ubagg_config *config)
{
    struct ubagg_rmv_dev args = {0};
    struct ubagg_cmd_hdr hdr = {0};
    int dev_fd;
    int ret;

    if (config->type != CONFIG_RMV_DEV) {
        fprintf(stderr, "wrong operation type.\n");
        return -1;
    }

    config->master_dev_name[MAX_DEV_NAME - 1] = '\0';
    (void)strncpy(args.in.master_dev_name, config->master_dev_name, MAX_DEV_NAME);

    hdr.command = UBAGG_RMV_DEV;
    hdr.args_len = sizeof(args);
    hdr.args_addr = (uint64_t)&args;

    dev_fd = open("/dev/ubagg", O_RDWR);
    if (dev_fd == -1) {
        fprintf(stderr, "Failed to open dev_fd err: %s.\n", ub_strerror(errno));
        return -1;
    }

    ret = ioctl(dev_fd, UBAGG_CMD, &hdr);
    if (ret != 0) {
        fprintf(stderr, "ioctl to remove ubagg device fail\n");
        close(dev_fd);
        return -1;
    }

    close(dev_fd);
    return 0;
}

int main(int argc, char *argv[])
{
    struct ubagg_config config;
    int ret = 0;

    (void)memset(&config, 0, sizeof(struct ubagg_config));

    ret = parse_args(argc, argv, &config);
    if (ret != 0) {
        fprintf(stderr, "parse args fail.\n");
        return ret;
    }

    if (config.type == CONFIG_ADD_DEV) {
        ret = ubagg_ioctl_add_dev(&config);
    } else if (config.type == CONFIG_RMV_DEV) {
        ret = ubagg_ioctl_rmv_dev(&config);
    }
    return ret;
}
