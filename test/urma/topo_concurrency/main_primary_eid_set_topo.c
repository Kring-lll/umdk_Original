/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal manual topo setter for main UE EID performance tests.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvs_api.h"

#define DEFAULT_DEV_IDX 255U
#define DEFAULT_NODE_ID 0U
#define DEFAULT_ENTITY_ID 0U
#define DEFAULT_CHIP_ID 0U
#define DEFAULT_DIE_ID 0U

#define DEFAULT_SYNTHETIC_DEV_IDX 0U
#define DEFAULT_SYNTHETIC_NODE_NUM 1U
#define DEFAULT_SYNTHETIC_NODE_ID_BASE 0U
#define DEFAULT_SYNTHETIC_SUPER_NODE_ID 0U
#define DEFAULT_SYNTHETIC_ENTITY_ID_BASE 0U
#define DEFAULT_SYNTHETIC_ENTITY_NUM 8U
#define DEFAULT_SYNTHETIC_DEVS_PER_ENTITY 6U
#define DEFAULT_SYNTHETIC_PORTS_PER_UE 4U
#define SYNTHETIC_EID_KIND_PRIMARY 1U
#define SYNTHETIC_EID_KIND_PORT 2U
#define SYNTHETIC_EID_KIND_AGG 3U
#define SYNTHETIC_EID_PRIMARY_PORT_TAG 0xffU
#define SYNTHETIC_EID_AGG_PORT_TAG 0xfeU
#define MAX_REAL_PRIMARY_EID_NUM (DEV_NUM * IODIE_NUM)
#define TOPO_PREVIEW_MAX_NODES 4U
#define TOPO_PREVIEW_MAX_DEVS_PER_NODE 12U
#define TOPO_PREVIEW_MAX_PORTS_PER_UE 4U

struct synthetic_topo_cfg {
    uint32_t node_num;
    uint32_t node_id_base;
    uint32_t super_node_id;
    uint32_t entity_id_base;
    uint32_t entity_num;
    uint32_t devs_per_entity;
    uint32_t ports_per_ue;
};

struct topo_map_debug {
    struct urma_topo_node topo_infos[MAX_NODE_NUM];
    uint32_t node_num;
};

struct expected_topo_file_header {
    char magic[8];
    uint32_t version;
    uint32_t node_size;
    uint32_t node_num;
};

#define EXPECTED_TOPO_FILE_MAGIC "MUEETP1"
#define EXPECTED_TOPO_FILE_VERSION 1U

static uint32_t max_u32(uint32_t left, uint32_t right)
{
    return left > right ? left : right;
}

static int parse_eid(const char *str, uvs_eid_t *eid)
{
    if (str == NULL || eid == NULL) {
        return -EINVAL;
    }

    (void)memset(eid, 0, sizeof(*eid));
    if (inet_pton(AF_INET6, str, eid->raw) != 1) {
        fprintf(stderr, "invalid IPv6 EID: %s\n", str);
        return -EINVAL;
    }

    return 0;
}

static int parse_u32(const char *name, const char *value, uint32_t *out)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -EINVAL;
    }

    *out = (uint32_t)parsed;
    return 0;
}

static void eid_to_str(const uvs_eid_t *eid, char *buf, size_t len)
{
    if (inet_ntop(AF_INET6, eid->raw, buf, len) == NULL) {
        (void)snprintf(buf, len, "<invalid>");
    }
}

static bool raw_eid_valid(const char *raw)
{
    const char empty[EID_LEN] = {0};

    return raw != NULL && memcmp(raw, empty, EID_LEN) != 0;
}

static void raw_eid_to_str(const char *raw, char *buf, size_t len)
{
    if (!raw_eid_valid(raw)) {
        (void)snprintf(buf, len, "<empty>");
        return;
    }

    if (inet_ntop(AF_INET6, raw, buf, len) == NULL) {
        (void)snprintf(buf, len, "<invalid>");
    }
}

static uint32_t count_valid_ports(const struct urma_topo_ue *ue)
{
    uint32_t port_idx;
    uint32_t port_num = 0;

    for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
        if (raw_eid_valid(ue->port_eid[port_idx])) {
            port_num++;
        }
    }

    return port_num;
}

static void print_topo_preview(const char *title, const struct urma_topo_node *topo,
                               uint32_t node_num)
{
    uint32_t node_idx;
    uint32_t total_valid_devs = 0;
    uint32_t total_valid_ues = 0;
    uint32_t total_valid_ports = 0;
    uint32_t printed_nodes = 0;

    printf("\n%s\n", title);
    printf("topo node_num=%u, preview max nodes=%u devs_per_node=%u ports_per_ue=%u\n",
           node_num, TOPO_PREVIEW_MAX_NODES, TOPO_PREVIEW_MAX_DEVS_PER_NODE,
           TOPO_PREVIEW_MAX_PORTS_PER_UE);

    for (node_idx = 0; node_idx < node_num; node_idx++) {
        const struct urma_topo_node *node = &topo[node_idx];
        uint32_t printed_devs = 0;
        uint32_t dev_idx;

        if (printed_nodes < TOPO_PREVIEW_MAX_NODES) {
            printf("node[%u]: node_id=%u super_node_id=%u type=%u is_current=%u\n",
                   node_idx, node->node_id, node->super_node_id,
                   node->type, node->is_current);
            printed_nodes++;
        }

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];
            char agg_buf[INET6_ADDRSTRLEN] = {0};
            uint32_t die_idx;
            bool dev_has_ue = false;
            bool preview_dev;

            if (!raw_eid_valid(agg_dev->agg_eid)) {
                continue;
            }

            total_valid_devs++;
            raw_eid_to_str(agg_dev->agg_eid, agg_buf, sizeof(agg_buf));
            preview_dev = node_idx < TOPO_PREVIEW_MAX_NODES &&
                          printed_devs < TOPO_PREVIEW_MAX_DEVS_PER_NODE;
            if (preview_dev) {
                printf("  dev[%u]: agg_eid=%s\n", dev_idx, agg_buf);
                printed_devs++;
            }

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                const struct urma_topo_ue *ue = &agg_dev->ues[die_idx];
                char primary_buf[INET6_ADDRSTRLEN] = {0};
                uint32_t port_idx;
                uint32_t port_num;
                uint32_t printed_ports = 0;

                if (!raw_eid_valid(ue->primary_eid)) {
                    continue;
                }

                dev_has_ue = true;
                total_valid_ues++;
                port_num = count_valid_ports(ue);
                total_valid_ports += port_num;
                raw_eid_to_str(ue->primary_eid, primary_buf,
                               sizeof(primary_buf));

                if (preview_dev) {
                    printf("    ue[die=%u]: entity=%u chip=%u primary=%s port_num=%u\n",
                           die_idx, ue->entity_id, ue->chip_id,
                           primary_buf, port_num);
                }

                for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
                    char port_buf[INET6_ADDRSTRLEN] = {0};

                    if (!raw_eid_valid(ue->port_eid[port_idx])) {
                        continue;
                    }
                    if (!preview_dev ||
                        printed_ports >= TOPO_PREVIEW_MAX_PORTS_PER_UE) {
                        printed_ports++;
                        continue;
                    }
                    raw_eid_to_str(ue->port_eid[port_idx], port_buf,
                                   sizeof(port_buf));
                    printf("      port[%u]=%s\n", port_idx, port_buf);
                    printed_ports++;
                }
                if (printed_ports > TOPO_PREVIEW_MAX_PORTS_PER_UE &&
                    preview_dev) {
                    printf("      ... %u more port EIDs omitted\n",
                           printed_ports - TOPO_PREVIEW_MAX_PORTS_PER_UE);
                }
            }

            if (!dev_has_ue && preview_dev) {
                printf("    no valid UE primary EID under this dev\n");
            }
        }

        if (printed_devs == TOPO_PREVIEW_MAX_DEVS_PER_NODE) {
            printf("  ... more devs may be omitted for node[%u]\n", node_idx);
        }
    }

    printf("topo preview summary: valid_devs=%u valid_ues=%u valid_ports=%u\n\n",
           total_valid_devs, total_valid_ues, total_valid_ports);
}

static struct topo_map_debug *load_existing_ubcore_topo(void)
{
    struct topo_map_debug *topo_map = calloc(1, sizeof(*topo_map));
    int ret;

    if (topo_map == NULL) {
        return NULL;
    }

    ret = uvs_get_topo_info(topo_map);
    if (ret != 0) {
        free(topo_map);
        return NULL;
    }
    if (topo_map->node_num == 0 || topo_map->node_num > MAX_NODE_NUM) {
        free(topo_map);
        return NULL;
    }

    return topo_map;
}

static void print_existing_ubcore_topo(void)
{
    struct topo_map_debug *topo_map = load_existing_ubcore_topo();

    if (topo_map == NULL) {
        printf("\nexisting ubcore topo before set: unavailable\n");
        printf("this only means ubcore has no readable topo or get_topo failed; ubagg may still have its own topo\n\n");
        return;
    }

    print_topo_preview("existing ubcore topo before set",
                       topo_map->topo_infos, topo_map->node_num);
    free(topo_map);
}

static int dump_expected_topo_file(const char *path,
                                   const struct urma_topo_node *topo,
                                   uint32_t node_num)
{
    struct expected_topo_file_header header = {
        .magic = EXPECTED_TOPO_FILE_MAGIC,
        .version = EXPECTED_TOPO_FILE_VERSION,
        .node_size = sizeof(*topo),
        .node_num = node_num,
    };
    FILE *fp;

    if (path == NULL) {
        return 0;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open expected topo file %s: %s\n",
                path, strerror(errno));
        return -errno;
    }

    if (fwrite(&header, sizeof(header), 1, fp) != 1 ||
        fwrite(topo, sizeof(*topo), node_num, fp) != node_num) {
        int err = ferror(fp) ? errno : EIO;

        (void)fclose(fp);
        fprintf(stderr, "failed to write expected topo file %s: %s\n",
                path, strerror(err));
        return -err;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "failed to close expected topo file %s: %s\n",
                path, strerror(errno));
        return -errno;
    }

    printf("dumped old-topo expected input to %s\n", path);
    return 0;
}

static void derive_agg_eid(const uvs_eid_t *primary, uvs_eid_t *agg)
{
    *agg = *primary;
    agg->raw[15] ^= 0x80;
    if (memcmp(agg->raw, primary->raw, EID_LEN) == 0) {
        agg->raw[15] ^= 0x01;
    }
}

static void init_links(struct urma_topo_node *node)
{
    uint32_t iodie_idx;
    uint32_t port_idx;

    for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
        for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
            node->links[iodie_idx][port_idx].peer_node = UINT32_MAX;
            node->links[iodie_idx][port_idx].peer_iodie = UINT32_MAX;
            node->links[iodie_idx][port_idx].peer_port = UINT32_MAX;
        }
    }
}

static void put_be16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value >> 8);
    buf[1] = (uint8_t)value;
}

static void put_be32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value >> 24);
    buf[1] = (uint8_t)(value >> 16);
    buf[2] = (uint8_t)(value >> 8);
    buf[3] = (uint8_t)value;
}

static void make_synthetic_eid(uint8_t kind, uint32_t node_id,
                               uint32_t entity_id, uint32_t dev_idx,
                               uint32_t replica, uint32_t chip_id,
                               uint32_t die_id, uint32_t port_idx,
                               uvs_eid_t *eid)
{
    (void)memset(eid, 0, sizeof(*eid));

    eid->raw[0] = 0xfd;
    eid->raw[1] = 0x42;
    eid->raw[2] = kind;
    eid->raw[3] = 0;
    put_be32(&eid->raw[4], node_id);
    put_be16(&eid->raw[8], (uint16_t)entity_id);
    eid->raw[10] = (uint8_t)chip_id;
    eid->raw[11] = (uint8_t)replica;
    eid->raw[12] = (uint8_t)port_idx;
    eid->raw[13] = (uint8_t)die_id;
    put_be16(&eid->raw[14], (uint16_t)dev_idx);
}

static uint32_t synthetic_current_port_num(const struct synthetic_topo_cfg *cfg,
                                           uint32_t explicit_port_num)
{
    return max_u32(cfg->ports_per_ue, explicit_port_num);
}

static uint32_t div_round_up_u32(uint32_t value, uint32_t divisor)
{
    return (value + divisor - 1) / divisor;
}

static bool eid_equal(const uvs_eid_t *left, const uvs_eid_t *right)
{
    return memcmp(left->raw, right->raw, EID_LEN) == 0;
}

static int validate_unique_primary_eids(const uvs_eid_t *primary_eids,
                                        uint32_t primary_eid_num)
{
    uint32_t i;
    uint32_t j;

    for (i = 0; i < primary_eid_num; i++) {
        for (j = 0; j < i; j++) {
            if (eid_equal(&primary_eids[i], &primary_eids[j])) {
                fprintf(stderr, "duplicated --primary-eid at index %u and %u\n",
                        j, i);
                return -EEXIST;
            }
        }
    }

    return 0;
}

static uint32_t synthetic_current_group_count(const struct synthetic_topo_cfg *cfg,
                                              uint32_t primary_eid_num)
{
    if (primary_eid_num == 0) {
        return 0;
    }

    return div_round_up_u32(primary_eid_num, cfg->devs_per_entity);
}

static uint32_t synthetic_current_dev_slot_count(const struct synthetic_topo_cfg *cfg,
                                                 uint32_t primary_eid_num)
{
    uint32_t group_num;
    uint32_t entity_group_num;
    uint32_t last_entity_group_idx;
    uint32_t group_num_before_last_entity;
    uint32_t group_num_in_last_entity;
    uint32_t last_group_idx;
    uint32_t used_in_last_group;

    if (primary_eid_num == 0) {
        return 0;
    }

    group_num = synthetic_current_group_count(cfg, primary_eid_num);
    entity_group_num = div_round_up_u32(group_num, CHIP_NUM);
    last_entity_group_idx = entity_group_num - 1;
    group_num_before_last_entity = last_entity_group_idx * CHIP_NUM;
    group_num_in_last_entity = group_num - group_num_before_last_entity;
    last_group_idx = group_num - 1;
    used_in_last_group = primary_eid_num - last_group_idx * cfg->devs_per_entity;

    if (group_num_in_last_entity == CHIP_NUM) {
        return entity_group_num * cfg->devs_per_entity;
    }

    return last_entity_group_idx * cfg->devs_per_entity + used_in_last_group;
}

static uint64_t synthetic_current_entry_count(const struct synthetic_topo_cfg *cfg,
                                              uint32_t primary_eid_num,
                                              uint32_t explicit_port_num)
{
    uint32_t first_port_num;

    if (primary_eid_num == 0) {
        return 0;
    }

    first_port_num = synthetic_current_port_num(cfg, explicit_port_num);
    return (uint64_t)primary_eid_num * (1U + cfg->ports_per_ue) +
           max_u32(first_port_num, cfg->ports_per_ue) - cfg->ports_per_ue;
}

static int validate_synthetic_cfg(const struct synthetic_topo_cfg *cfg,
                                  uint32_t primary_eid_num,
                                  uint32_t dev_idx,
                                  uint32_t entity_id,
                                  uint32_t explicit_port_num)
{
    uint64_t dev_num;
    uint32_t current_group_num;
    uint32_t current_dev_num;

    if (cfg->node_num == 0 || cfg->node_num > MAX_NODE_NUM) {
        fprintf(stderr, "synthetic node-num out of range, max %u\n",
                MAX_NODE_NUM);
        return -EINVAL;
    }
    if (cfg->node_num - 1 > UINT32_MAX - cfg->node_id_base) {
        fprintf(stderr, "synthetic node ids overflow uint32 range\n");
        return -EINVAL;
    }
    if (cfg->entity_num == 0 || cfg->devs_per_entity == 0) {
        fprintf(stderr, "synthetic entity-num/devs-per-entity must be nonzero\n");
        return -EINVAL;
    }
    if (primary_eid_num == 0 || primary_eid_num > MAX_REAL_PRIMARY_EID_NUM) {
        fprintf(stderr, "synthetic primary EID count out of range, max %u\n",
                MAX_REAL_PRIMARY_EID_NUM);
        return -EINVAL;
    }
    if (cfg->ports_per_ue > PORT_NUM) {
        fprintf(stderr, "synthetic ports-per-ue out of range, max %u\n",
                PORT_NUM);
        return -EINVAL;
    }
    if (explicit_port_num > PORT_NUM) {
        fprintf(stderr, "synthetic explicit port EID num out of range, max %u\n",
                PORT_NUM);
        return -EINVAL;
    }
    dev_num = (uint64_t)cfg->entity_num * cfg->devs_per_entity;
    if (dev_num > DEV_NUM) {
        fprintf(stderr, "synthetic entity-num * devs-per-entity exceeds DEV_NUM %u\n",
                DEV_NUM);
        return -EINVAL;
    }

    current_group_num = synthetic_current_group_count(cfg, primary_eid_num);
    if (current_group_num > cfg->entity_num * CHIP_NUM) {
        fprintf(stderr,
                "primary EID count %u needs %u current groups, but entity-num * CHIP_NUM is %u\n",
                primary_eid_num, current_group_num,
                cfg->entity_num * CHIP_NUM);
        return -EINVAL;
    }
    if (cfg->entity_id_base > UINT16_MAX ||
        cfg->entity_num - 1 > UINT16_MAX - cfg->entity_id_base) {
        fprintf(stderr, "synthetic non-current entity ids must fit in 16 bits for EID encoding\n");
        return -EINVAL;
    }
    if (current_group_num != 0) {
        uint32_t current_entity_num = div_round_up_u32(current_group_num, CHIP_NUM);

        if (entity_id > UINT16_MAX - (current_entity_num - 1)) {
            fprintf(stderr, "synthetic current-node entity ids must fit in 16 bits for EID encoding\n");
            return -EINVAL;
        }
    }

    current_dev_num = synthetic_current_dev_slot_count(cfg, primary_eid_num);
    if (dev_idx >= DEV_NUM || current_dev_num > DEV_NUM - dev_idx) {
        fprintf(stderr,
                "primary EID layout needs %u dev slots from dev_idx %u, DEV_NUM is %u\n",
                current_dev_num, dev_idx, DEV_NUM);
        return -EINVAL;
    }

    return 0;
}

static void fill_synthetic_current_node(struct urma_topo_node *node,
                                        const struct synthetic_topo_cfg *cfg,
                                        const uvs_eid_t *primary_eids,
                                        uint32_t primary_eid_num,
                                        const uvs_eid_t *manual_agg_eid,
                                        bool has_manual_agg_eid,
                                        const uvs_eid_t *port_eids,
                                        uint32_t explicit_port_num,
                                        uint32_t dev_idx,
                                        uint32_t entity_id,
                                        uint32_t chip_id,
                                        uint32_t die_id)
{
    uint32_t primary_idx;

    node->type = UVS_TOPO_TYPE_FULLMESH_1D;
    node->super_node_id = cfg->super_node_id;
    node->node_id = cfg->node_id_base;
    node->is_current = 1;
    init_links(node);

    for (primary_idx = 0; primary_idx < primary_eid_num; primary_idx++) {
        uint32_t group_idx = primary_idx / cfg->devs_per_entity;
        uint32_t replica = primary_idx % cfg->devs_per_entity;
        uint32_t entity_group = group_idx / CHIP_NUM;
        uint32_t chip_group = group_idx % CHIP_NUM;
        uint32_t cur_entity_id = entity_id + entity_group;
        uint32_t cur_chip_id = (chip_id + chip_group) % CHIP_NUM;
        uint32_t cur_die_id = (die_id + chip_group) % IODIE_NUM;
        uint32_t cur_dev_idx = dev_idx + entity_group * cfg->devs_per_entity +
                               replica;
        uint32_t port_num = cfg->ports_per_ue;
        struct urma_topo_ue *ue = &node->agg_devs[cur_dev_idx].ues[cur_die_id];
        uvs_eid_t agg_eid = {0};
        uint32_t port_idx;

        if (primary_idx == 0 && has_manual_agg_eid) {
            agg_eid = *manual_agg_eid;
        } else {
            derive_agg_eid(&primary_eids[primary_idx], &agg_eid);
        }
        (void)memcpy(node->agg_devs[cur_dev_idx].agg_eid, agg_eid.raw, EID_LEN);

        ue->chip_id = cur_chip_id;
        ue->die_id = cur_die_id;
        ue->entity_id = cur_entity_id;
        (void)memcpy(ue->primary_eid, primary_eids[primary_idx].raw, EID_LEN);

        if (primary_idx == 0) {
            port_num = synthetic_current_port_num(cfg, explicit_port_num);
        }

        for (port_idx = 0; port_idx < port_num; port_idx++) {
            uvs_eid_t port_eid = {0};

            if (primary_idx == 0 && port_idx < explicit_port_num) {
                (void)memcpy(ue->port_eid[port_idx],
                             port_eids[port_idx].raw, EID_LEN);
                continue;
            }

            make_synthetic_eid(SYNTHETIC_EID_KIND_PORT, cfg->node_id_base,
                               cur_entity_id, cur_dev_idx, replica,
                               cur_chip_id, cur_die_id, port_idx, &port_eid);
            (void)memcpy(ue->port_eid[port_idx], port_eid.raw, EID_LEN);
        }
    }
}

static void fill_synthetic_topo_node(struct urma_topo_node *node,
                                     const struct synthetic_topo_cfg *cfg,
                                     uint32_t node_idx)
{
    uint32_t node_id = cfg->node_id_base + node_idx;
    uint32_t entity_idx;
    uint32_t replica;
    uint32_t die_idx;
    uint32_t port_idx;

    node->type = UVS_TOPO_TYPE_FULLMESH_1D;
    node->super_node_id = cfg->super_node_id;
    node->node_id = node_id;
    node->is_current = 0;
    init_links(node);

    for (entity_idx = 0; entity_idx < cfg->entity_num; entity_idx++) {
        uint32_t entity_id = cfg->entity_id_base + entity_idx;

        for (replica = 0; replica < cfg->devs_per_entity; replica++) {
            uint32_t dev_idx = entity_idx * cfg->devs_per_entity + replica;
            uvs_eid_t agg_eid = {0};

            make_synthetic_eid(SYNTHETIC_EID_KIND_AGG, node_id, entity_id,
                               dev_idx, replica, UINT8_MAX, UINT8_MAX,
                               SYNTHETIC_EID_AGG_PORT_TAG, &agg_eid);
            (void)memcpy(node->agg_devs[dev_idx].agg_eid, agg_eid.raw,
                         EID_LEN);

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                struct urma_topo_ue *ue = &node->agg_devs[dev_idx].ues[die_idx];
                uint32_t chip_id = die_idx % CHIP_NUM;
                uvs_eid_t primary_eid = {0};

                ue->chip_id = chip_id;
                ue->die_id = die_idx;
                ue->entity_id = entity_id;
                make_synthetic_eid(SYNTHETIC_EID_KIND_PRIMARY, node_id,
                                   entity_id, dev_idx, replica, chip_id,
                                   die_idx, SYNTHETIC_EID_PRIMARY_PORT_TAG,
                                   &primary_eid);
                (void)memcpy(ue->primary_eid, primary_eid.raw, EID_LEN);

                for (port_idx = 0; port_idx < cfg->ports_per_ue; port_idx++) {
                    uvs_eid_t port_eid = {0};

                    make_synthetic_eid(SYNTHETIC_EID_KIND_PORT, node_id,
                                       entity_id, dev_idx, replica, chip_id,
                                       die_idx, port_idx, &port_eid);
                    (void)memcpy(ue->port_eid[port_idx], port_eid.raw,
                                 EID_LEN);
                }
            }
        }
    }
}

static int set_synthetic_topo(const struct synthetic_topo_cfg *cfg,
                              const uvs_eid_t *primary_eids,
                              uint32_t primary_eid_num,
                              const uvs_eid_t *manual_agg_eid,
                              bool has_manual_agg_eid,
                              const uvs_eid_t *port_eids,
                              uint32_t explicit_port_num,
                              uint32_t dev_idx,
                              uint32_t entity_id,
                              uint32_t chip_id,
                              uint32_t die_id,
                              const char *dump_topo_file)
{
    struct topo_map_debug *existing_topo = NULL;
    struct urma_topo_node *topo = NULL;
    uint64_t entry_num;
    uint64_t group_num;
    uint64_t primary_num;
    uint32_t preserved_node_num = 0;
    uint32_t fake_node_begin = 1;
    uint32_t node_idx;
    int ret;

    ret = validate_unique_primary_eids(primary_eids, primary_eid_num);
    if (ret != 0) {
        return ret;
    }

    ret = validate_synthetic_cfg(cfg, primary_eid_num, dev_idx, entity_id,
                                 explicit_port_num);
    if (ret != 0) {
        return ret;
    }

    existing_topo = load_existing_ubcore_topo();
    if (existing_topo != NULL) {
        preserved_node_num = existing_topo->node_num;
        fake_node_begin = preserved_node_num;
        if (preserved_node_num >= cfg->node_num) {
            fprintf(stderr,
                    "synthetic node-num %u must be larger than existing ubcore topo node_num %u\n",
                    cfg->node_num, preserved_node_num);
            free(existing_topo);
            return -EINVAL;
        }
    }

    topo = calloc(cfg->node_num, sizeof(*topo));
    if (topo == NULL) {
        free(existing_topo);
        return -ENOMEM;
    }

    if (existing_topo != NULL) {
        (void)memcpy(topo, existing_topo->topo_infos,
                     sizeof(*topo) * preserved_node_num);
    } else {
        fill_synthetic_current_node(&topo[0], cfg, primary_eids,
                                    primary_eid_num, manual_agg_eid,
                                    has_manual_agg_eid, port_eids,
                                    explicit_port_num, dev_idx, entity_id,
                                    chip_id, die_id);
    }

    for (node_idx = fake_node_begin; node_idx < cfg->node_num; node_idx++) {
        fill_synthetic_topo_node(&topo[node_idx], cfg, node_idx);
    }
    free(existing_topo);

    group_num = (uint64_t)(cfg->node_num - fake_node_begin) *
                cfg->entity_num * IODIE_NUM;
    primary_num = (uint64_t)(cfg->node_num - fake_node_begin) *
                  cfg->entity_num * cfg->devs_per_entity * IODIE_NUM;
    entry_num = (uint64_t)(cfg->node_num - fake_node_begin) *
                cfg->entity_num * cfg->devs_per_entity * IODIE_NUM *
                (1U + cfg->ports_per_ue);
    if (preserved_node_num == 0) {
        group_num += synthetic_current_group_count(cfg, primary_eid_num);
        primary_num += primary_eid_num;
        entry_num += synthetic_current_entry_count(cfg, primary_eid_num,
                                                   explicit_port_num);
    }

    printf("set synthetic topo: nodes=%u node_id_base=%u entity_base=%u\n",
           cfg->node_num, cfg->node_id_base, cfg->entity_id_base);
    if (preserved_node_num != 0) {
        printf("preserve existing ubcore topo nodes=%u, append fake non-current nodes=%u\n",
               preserved_node_num, cfg->node_num - preserved_node_num);
    } else {
        printf("current_node uses %u real primary_eids, dev_idx_base=%u die_base=%u entity_base=%u chip_base=%u\n",
               primary_eid_num, dev_idx, die_id, entity_id, chip_id);
    }
    printf("non_current_entities_per_node=%u chips_per_dev=%u devs_per_entity=%u ports_per_ue=%u\n",
           cfg->entity_num, IODIE_NUM, cfg->devs_per_entity,
           cfg->ports_per_ue);
    printf("main-ue groups=%" PRIu64 " primary_eids=%" PRIu64
           " table_entries=%" PRIu64 "\n", group_num, primary_num, entry_num);
    print_existing_ubcore_topo();
    print_topo_preview("topo preview before uvs_set_topo_info", topo,
                       cfg->node_num);

    ret = uvs_set_topo_info(topo, sizeof(*topo), cfg->node_num);
    if (ret != 0) {
        free(topo);
        fprintf(stderr, "uvs_set_topo_info failed, ret=%d\n", ret);
        return ret;
    }

    ret = dump_expected_topo_file(dump_topo_file, topo, cfg->node_num);
    free(topo);
    if (ret != 0) {
        return ret;
    }

    printf("synthetic topo set successfully\n");
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s --primary-eid <eid> [--synthetic] [options]\n", prog);
    printf("Options:\n");
    printf("  --primary-eid <eid>  Existing UB device primary EID, repeatable in synthetic mode\n");
    printf("  --agg-eid <eid>      Aggregation EID, default derived from primary EID\n");
    printf("  --port-eid <eid>     Add one port EID, repeatable up to %u\n", PORT_NUM);
    printf("  --dev-idx <idx>      agg_devs index, default %u\n", DEFAULT_DEV_IDX);
    printf("  --node-id <id>       node_id, default %u\n", DEFAULT_NODE_ID);
    printf("  --entity-id <id>     entity_id, default %u\n", DEFAULT_ENTITY_ID);
    printf("  --chip-id <id>       chip_id, default %u\n", DEFAULT_CHIP_ID);
    printf("  --die-id <id>        die_id, default %u\n", DEFAULT_DIE_ID);
    printf("Synthetic topo options:\n");
    printf("  --synthetic              Generate diversified non-current nodes; current node still uses --primary-eid\n");
    printf("  --dev-idx <idx>          Synthetic current-node agg_devs base index, default %u\n",
           DEFAULT_SYNTHETIC_DEV_IDX);
    printf("  --node-num <num>         Synthetic node count, default %u\n",
           DEFAULT_SYNTHETIC_NODE_NUM);
    printf("  --node-id-base <id>      Synthetic node_id base, default %u\n",
           DEFAULT_SYNTHETIC_NODE_ID_BASE);
    printf("  --super-node-id <id>     Synthetic super_node_id, default %u\n",
           DEFAULT_SYNTHETIC_SUPER_NODE_ID);
    printf("  --entity-id-base <id>    Synthetic entity_id base, default %u\n",
           DEFAULT_SYNTHETIC_ENTITY_ID_BASE);
    printf("  --entity-num <num>       Synthetic entities per node, default %u\n",
           DEFAULT_SYNTHETIC_ENTITY_NUM);
    printf("  --devs-per-entity <num>  Primary EIDs per node/entity/chip group, default %u\n",
           DEFAULT_SYNTHETIC_DEVS_PER_ENTITY);
    printf("  --ports-per-ue <num>     Port EIDs per UE, default %u\n",
           DEFAULT_SYNTHETIC_PORTS_PER_UE);
    printf("  --dump-topo <file>       Dump generated topo for old-topo expected lookup\n");
    printf("  --flush-first            Flush main_ue_eid table before set_topo\n");
}

int main(int argc, char **argv)
{
    struct urma_topo_node *topo = NULL;
    uvs_eid_t primary_eids[MAX_REAL_PRIMARY_EID_NUM] = {0};
    uvs_eid_t agg_eid = {0};
    uvs_eid_t port_eids[PORT_NUM] = {0};
    uint32_t primary_eid_num = 0;
    uint32_t port_num = 0;
    uint32_t dev_idx = DEFAULT_DEV_IDX;
    uint32_t node_id = DEFAULT_NODE_ID;
    uint32_t entity_id = DEFAULT_ENTITY_ID;
    uint32_t chip_id = DEFAULT_CHIP_ID;
    uint32_t die_id = DEFAULT_DIE_ID;
    bool dev_idx_set = false;
    bool has_agg = false;
    bool synthetic = false;
    bool flush_first = false;
    const char *dump_topo_file = NULL;
    struct synthetic_topo_cfg synthetic_cfg = {
        .node_num = DEFAULT_SYNTHETIC_NODE_NUM,
        .node_id_base = DEFAULT_SYNTHETIC_NODE_ID_BASE,
        .super_node_id = DEFAULT_SYNTHETIC_SUPER_NODE_ID,
        .entity_id_base = DEFAULT_SYNTHETIC_ENTITY_ID_BASE,
        .entity_num = DEFAULT_SYNTHETIC_ENTITY_NUM,
        .devs_per_entity = DEFAULT_SYNTHETIC_DEVS_PER_ENTITY,
        .ports_per_ue = DEFAULT_SYNTHETIC_PORTS_PER_UE,
    };
    char primary_str[INET6_ADDRSTRLEN] = {0};
    char agg_str[INET6_ADDRSTRLEN] = {0};
    uint32_t i;
    int ret;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (strcmp(argv[i], "--primary-eid") == 0 && i + 1 < (uint32_t)argc) {
            if (primary_eid_num >= MAX_REAL_PRIMARY_EID_NUM) {
                fprintf(stderr, "too many --primary-eid values, max %u\n",
                        MAX_REAL_PRIMARY_EID_NUM);
                return EINVAL;
            }
            ret = parse_eid(argv[++i], &primary_eids[primary_eid_num]);
            if (ret != 0) {
                return -ret;
            }
            primary_eid_num++;
        } else if (strcmp(argv[i], "--agg-eid") == 0 && i + 1 < (uint32_t)argc) {
            ret = parse_eid(argv[++i], &agg_eid);
            if (ret != 0) {
                return -ret;
            }
            has_agg = true;
        } else if (strcmp(argv[i], "--port-eid") == 0 && i + 1 < (uint32_t)argc) {
            if (port_num >= PORT_NUM) {
                fprintf(stderr, "too many --port-eid values, max %u\n", PORT_NUM);
                return EINVAL;
            }
            ret = parse_eid(argv[++i], &port_eids[port_num]);
            if (ret != 0) {
                return -ret;
            }
            port_num++;
        } else if (strcmp(argv[i], "--dev-idx") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--dev-idx", argv[++i], &dev_idx) != 0) {
                return EINVAL;
            }
            dev_idx_set = true;
        } else if (strcmp(argv[i], "--node-id") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--node-id", argv[++i], &node_id) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--entity-id") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--entity-id", argv[++i], &entity_id) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--chip-id") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--chip-id", argv[++i], &chip_id) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--die-id") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--die-id", argv[++i], &die_id) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--synthetic") == 0) {
            synthetic = true;
        } else if (strcmp(argv[i], "--node-num") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--node-num", argv[++i], &synthetic_cfg.node_num) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--node-id-base") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--node-id-base", argv[++i], &synthetic_cfg.node_id_base) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--super-node-id") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--super-node-id", argv[++i], &synthetic_cfg.super_node_id) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--entity-id-base") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--entity-id-base", argv[++i],
                          &synthetic_cfg.entity_id_base) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--entity-num") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--entity-num", argv[++i], &synthetic_cfg.entity_num) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--devs-per-entity") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--devs-per-entity", argv[++i],
                          &synthetic_cfg.devs_per_entity) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--ports-per-ue") == 0 && i + 1 < (uint32_t)argc) {
            if (parse_u32("--ports-per-ue", argv[++i],
                          &synthetic_cfg.ports_per_ue) != 0) {
                return EINVAL;
            }
        } else if (strcmp(argv[i], "--dump-topo") == 0 && i + 1 < (uint32_t)argc) {
            dump_topo_file = argv[++i];
        } else if (strcmp(argv[i], "--flush-first") == 0) {
            flush_first = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return EINVAL;
        }
    }

    if (synthetic) {
        uint32_t synthetic_dev_idx = dev_idx_set ? dev_idx : DEFAULT_SYNTHETIC_DEV_IDX;

        if (primary_eid_num == 0) {
            fprintf(stderr,
                    "--synthetic requires --primary-eid because ubcore validates current-node primary EID during set_topo\n");
            return EINVAL;
        }
        if (synthetic_dev_idx >= DEV_NUM || die_id >= IODIE_NUM || chip_id >= CHIP_NUM) {
            fprintf(stderr, "dev_idx/die_id/chip_id out of range\n");
            return EINVAL;
        }
        if (!has_agg) {
            derive_agg_eid(&primary_eids[0], &agg_eid);
        }
        if (flush_first) {
            ret = uvs_flush_main_ue_eid();
            if (ret != 0) {
                fprintf(stderr, "flush main_ue_eid table failed, ret=%d\n",
                        ret);
                return -ret;
            }
            printf("flushed main_ue_eid table before synthetic set_topo\n");
        }
        ret = set_synthetic_topo(&synthetic_cfg, primary_eids,
                                 primary_eid_num, &agg_eid, has_agg,
                                 port_eids, port_num, synthetic_dev_idx,
                                 entity_id, chip_id, die_id, dump_topo_file);
        return ret == 0 ? 0 : -ret;
    }

    if (primary_eid_num != 1) {
        usage(argv[0]);
        if (primary_eid_num > 1) {
            fprintf(stderr, "manual mode supports exactly one --primary-eid; use --synthetic for multiple primary EIDs\n");
        }
        return EINVAL;
    }
    if (dev_idx >= DEV_NUM || die_id >= IODIE_NUM || chip_id >= CHIP_NUM) {
        fprintf(stderr, "dev_idx/die_id/chip_id out of range\n");
        return EINVAL;
    }
    if (!has_agg) {
        derive_agg_eid(&primary_eids[0], &agg_eid);
    }

    topo = calloc(1, sizeof(*topo));
    if (topo == NULL) {
        return ENOMEM;
    }

    topo->type = UVS_TOPO_TYPE_FULLMESH_1D;
    topo->super_node_id = 0;
    topo->node_id = node_id;
    topo->is_current = 1;
    init_links(topo);

    (void)memcpy(topo->agg_devs[dev_idx].agg_eid, agg_eid.raw, EID_LEN);
    topo->agg_devs[dev_idx].ues[die_id].chip_id = chip_id;
    topo->agg_devs[dev_idx].ues[die_id].die_id = die_id;
    topo->agg_devs[dev_idx].ues[die_id].entity_id = entity_id;
    (void)memcpy(topo->agg_devs[dev_idx].ues[die_id].primary_eid,
                 primary_eids[0].raw, EID_LEN);

    for (i = 0; i < port_num; i++) {
        (void)memcpy(topo->agg_devs[dev_idx].ues[die_id].port_eid[i],
                     port_eids[i].raw, EID_LEN);
    }

    eid_to_str(&primary_eids[0], primary_str, sizeof(primary_str));
    eid_to_str(&agg_eid, agg_str, sizeof(agg_str));
    printf("set manual topo: node=%u dev_idx=%u die=%u entity=%u chip=%u\n",
           node_id, dev_idx, die_id, entity_id, chip_id);
    printf("primary_eid=%s\n", primary_str);
    printf("agg_eid=%s\n", agg_str);
    printf("port_eid_num=%u\n", port_num);
    print_existing_ubcore_topo();
    print_topo_preview("topo preview before uvs_set_topo_info", topo, 1);

    if (flush_first) {
        ret = uvs_flush_main_ue_eid();
        if (ret != 0) {
            free(topo);
            fprintf(stderr, "flush main_ue_eid table failed, ret=%d\n", ret);
            return -ret;
        }
        printf("flushed main_ue_eid table before manual set_topo\n");
    }

    ret = uvs_set_topo_info(topo, sizeof(*topo), 1);
    if (ret != 0) {
        free(topo);
        fprintf(stderr, "uvs_set_topo_info failed, ret=%d\n", ret);
        fprintf(stderr, "The primary EID must already belong to a loaded UB device.\n");
        return -ret;
    }

    ret = dump_expected_topo_file(dump_topo_file, topo, 1);
    free(topo);
    if (ret != 0) {
        return -ret;
    }

    printf("manual topo set successfully\n");
    return 0;
}
