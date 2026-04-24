#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EID_LEN 16
#define PORT_NUM 9
#define DEV_NUM 256
#define IODIE_NUM 2

#define UVS_UBCORE_DEVICE_PATH "/dev/ubcore/ubcore"
#define TPSA_CMD_MAGIC 'V'

struct tpsa_cmd_hdr {
    uint32_t command;
    uint32_t args_len;
    uint64_t args_addr;
};

typedef enum uvs_global_cmd {
    UVS_CMD_SET_TOPO = 1,
    UVS_CMD_GET_TOPO_EID = 2,
    UVS_CMD_GET_TOPO = 3,
    UVS_CMD_GET_TOPO_PATH_EID = 4,
    UVS_CMD_GLOBAL_LAST
} uvs_global_cmd_t;

typedef struct uvs_set_topo {
    struct {
        void *topo_info;
        uint32_t topo_num;
    } in;
} uvs_set_topo_t;

#define TPSA_CMD _IOWR(TPSA_CMD_MAGIC, 1, struct tpsa_cmd_hdr)

struct urma_topo_ue {
    uint32_t chip_id;
    uint32_t die_id;
    uint32_t entity_id;
    char primary_eid[EID_LEN];
    char port_eid[PORT_NUM][EID_LEN];
};

struct urma_topo_agg_dev {
    char agg_eid[EID_LEN];
    struct urma_topo_ue ues[IODIE_NUM];
};

struct urma_topo_link {
    uint32_t peer_node;
    uint32_t peer_iodie;
    uint32_t peer_port;
};

struct urma_topo_node {
    uint32_t type;
    uint32_t super_node_id;
    uint32_t node_id;
    uint32_t is_current;
    struct urma_topo_link links[IODIE_NUM][PORT_NUM];
    struct urma_topo_agg_dev agg_devs[DEV_NUM];
};

struct tool_options {
    const char *input_path;
    uint32_t current_node;
    uint32_t topo_num;
    int print_only;
};

struct topo_text {
    struct urma_topo_node *nodes;
    uint32_t capacity;
    uint32_t count;
};

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --input <topo.txt> --current-node <node_id> [--count <node_num>] [--print-only]\n"
            "\n"
            "Input file format:\n"
            "  Simple text directives: node/devcode/ue/port_eid/endnode.\n",
            prog);
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static int parse_eid_ipv6(const char *text, char out[EID_LEN], const char *field_name, int line_no)
{
    char buf[40];
    int group_count = 0;
    int digit_count = 0;
    size_t len;
    size_t i;

    if (text == NULL) {
        fprintf(stderr, "line %d: missing %s\n", line_no, field_name);
        return -1;
    }

    len = strlen(text);
    if (len == 0 || len >= sizeof(buf)) {
        fprintf(stderr, "line %d: invalid ipv6 format for %s\n", line_no, field_name);
        return -1;
    }

    memcpy(buf, text, len + 1);
    for (i = 0; i < len; ++i) {
        char ch = buf[i];
        int is_hex = ((ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f') ||
                      (ch >= 'A' && ch <= 'F'));

        if (is_hex) {
            ++digit_count;
            if (digit_count > 4) {
                fprintf(stderr, "line %d: %s must use full ipv6 groups like 2001:0db8:0000:0000:0000:0000:0000:0001\n",
                        line_no, field_name);
                return -1;
            }
            continue;
        }

        if (ch != ':') {
            fprintf(stderr, "line %d: invalid ipv6 format for %s\n", line_no, field_name);
            return -1;
        }
        if (digit_count != 4) {
            fprintf(stderr, "line %d: %s must not use ipv6 abbreviation\n", line_no, field_name);
            return -1;
        }
        ++group_count;
        digit_count = 0;
    }

    if (digit_count != 4 || group_count != 7) {
        fprintf(stderr, "line %d: %s must be full 8-group ipv6 without abbreviation\n", line_no, field_name);
        return -1;
    }

    if (inet_pton(AF_INET6, text, out) != 1) {
        fprintf(stderr, "line %d: invalid ipv6 format for %s\n", line_no, field_name);
        return -1;
    }
    return 0;
}

static void generate_agg_eid(char out[EID_LEN], uint32_t device_code)
{
    uint8_t *raw = (uint8_t *)out;

    memset(out, 0, EID_LEN);
    raw[0] = (uint8_t)'U';
    raw[1] = (uint8_t)'A';
    raw[2] = (uint8_t)'G';
    raw[3] = (uint8_t)'G';
    raw[8] = (uint8_t)((device_code >> 24) & 0xff);
    raw[9] = (uint8_t)((device_code >> 16) & 0xff);
    raw[10] = (uint8_t)((device_code >> 8) & 0xff);
    raw[11] = (uint8_t)(device_code & 0xff);
}

static void format_eid_ipv6(const char eid[EID_LEN], char *buf, size_t len)
{
    const uint8_t *raw = (const uint8_t *)eid;

    (void)snprintf(buf, len,
                   "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                   raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                   raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
}

static int is_zero_eid(const char eid[EID_LEN])
{
    static const char zero_eid[EID_LEN] = {0};
    return memcmp(eid, zero_eid, EID_LEN) == 0;
}

static void init_topo_node(struct urma_topo_node *node)
{
    uint32_t i;
    uint32_t j;

    memset(node, 0, sizeof(*node));
    for (i = 0; i < IODIE_NUM; ++i) {
        for (j = 0; j < PORT_NUM; ++j) {
            node->links[i][j].peer_port = UINT32_MAX;
        }
    }
}

static int ensure_topo_capacity(struct topo_text *topo, uint32_t needed)
{
    struct urma_topo_node *new_nodes;
    uint32_t new_capacity;
    uint32_t i;

    if (needed <= topo->capacity) {
        return 0;
    }

    new_capacity = topo->capacity == 0 ? 4 : topo->capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT32_MAX / 2) {
            fprintf(stderr, "too many topo nodes\n");
            return -1;
        }
        new_capacity *= 2;
    }

    new_nodes = (struct urma_topo_node *)realloc(topo->nodes, (size_t)new_capacity * sizeof(*new_nodes));
    if (new_nodes == NULL) {
        fprintf(stderr, "realloc topo nodes failed\n");
        return -1;
    }

    topo->nodes = new_nodes;
    for (i = topo->capacity; i < new_capacity; ++i) {
        init_topo_node(&topo->nodes[i]);
    }
    topo->capacity = new_capacity;
    return 0;
}

static int split_line_tokens(char *line, char **tokens, int max_tokens)
{
    int count = 0;
    char *tok = strtok(line, " \t\r\n");

    while (tok != NULL) {
        if (count >= max_tokens) {
            return -1;
        }
        tokens[count++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }

    return count;
}

static int parse_node_line(char **tokens, int count, struct topo_text *topo,
                           struct urma_topo_node **current, int line_no)
{
    uint32_t type;
    uint32_t super_node_id;
    uint32_t node_id;
    uint32_t is_current;

    if (count != 5) {
        fprintf(stderr, "line %d: node expects 4 arguments\n", line_no);
        return -1;
    }
    if (parse_u32(tokens[1], &type) != 0 ||
        parse_u32(tokens[2], &super_node_id) != 0 ||
        parse_u32(tokens[3], &node_id) != 0 ||
        parse_u32(tokens[4], &is_current) != 0) {
        fprintf(stderr, "line %d: invalid node arguments\n", line_no);
        return -1;
    }

    if (ensure_topo_capacity(topo, topo->count + 1) != 0) {
        return -1;
    }

    *current = &topo->nodes[topo->count++];
    init_topo_node(*current);
    (*current)->type = type;
    (*current)->super_node_id = super_node_id;
    (*current)->node_id = node_id;
    (*current)->is_current = is_current;
    return 0;
}

static void apply_current_node(struct urma_topo_node *nodes, uint32_t topo_num, uint32_t current_node, int *found)
{
    uint32_t i;

    *found = 0;
    for (i = 0; i < topo_num; ++i) {
        nodes[i].is_current = (nodes[i].node_id == current_node) ? 1U : 0U;
        if (nodes[i].is_current != 0) {
            *found = 1;
        }
    }
}

static int parse_devcode_line(char **tokens, int count, struct urma_topo_node *current, int line_no)
{
    uint32_t dev_idx;
    uint32_t device_code;

    if (current == NULL) {
        fprintf(stderr, "line %d: devcode must appear after node\n", line_no);
        return -1;
    }
    if (count != 3) {
        fprintf(stderr, "line %d: devcode expects 2 arguments\n", line_no);
        return -1;
    }
    if (parse_u32(tokens[1], &dev_idx) != 0 ||
        parse_u32(tokens[2], &device_code) != 0 ||
        dev_idx >= DEV_NUM) {
        fprintf(stderr, "line %d: invalid devcode arguments\n", line_no);
        return -1;
    }

    generate_agg_eid(current->agg_devs[dev_idx].agg_eid, device_code);
    return 0;
}

static int parse_ue_line(char **tokens, int count, struct urma_topo_node *current, int line_no)
{
    uint32_t dev_idx;
    uint32_t ue_idx;
    uint32_t chip_id;
    uint32_t die_id;
    uint32_t entity_id;
    struct urma_topo_ue *ue;

    if (current == NULL) {
        fprintf(stderr, "line %d: ue must appear after node\n", line_no);
        return -1;
    }
    if (count != 7) {
        fprintf(stderr, "line %d: ue expects 6 arguments\n", line_no);
        return -1;
    }
    if (parse_u32(tokens[1], &dev_idx) != 0 ||
        parse_u32(tokens[2], &ue_idx) != 0 ||
        parse_u32(tokens[3], &chip_id) != 0 ||
        parse_u32(tokens[4], &die_id) != 0 ||
        parse_u32(tokens[5], &entity_id) != 0) {
        fprintf(stderr, "line %d: invalid ue arguments\n", line_no);
        return -1;
    }
    if (dev_idx >= DEV_NUM || ue_idx >= IODIE_NUM) {
        fprintf(stderr, "line %d: ue index out of range\n", line_no);
        return -1;
    }

    ue = &current->agg_devs[dev_idx].ues[ue_idx];
    ue->chip_id = chip_id;
    ue->die_id = die_id;
    ue->entity_id = entity_id;
    return parse_eid_ipv6(tokens[6], ue->primary_eid, "primary_eid", line_no);
}

static int parse_port_eid_line(char **tokens, int count, struct urma_topo_node *current, int line_no)
{
    uint32_t dev_idx;
    uint32_t ue_idx;
    uint32_t port_idx;

    if (current == NULL) {
        fprintf(stderr, "line %d: port_eid must appear after node\n", line_no);
        return -1;
    }
    if (count != 5) {
        fprintf(stderr, "line %d: port_eid expects 4 arguments\n", line_no);
        return -1;
    }
    if (parse_u32(tokens[1], &dev_idx) != 0 ||
        parse_u32(tokens[2], &ue_idx) != 0 ||
        parse_u32(tokens[3], &port_idx) != 0) {
        fprintf(stderr, "line %d: invalid port_eid arguments\n", line_no);
        return -1;
    }
    if (dev_idx >= DEV_NUM || ue_idx >= IODIE_NUM || port_idx >= PORT_NUM) {
        fprintf(stderr, "line %d: port_eid index out of range\n", line_no);
        return -1;
    }

    return parse_eid_ipv6(tokens[4],
                          current->agg_devs[dev_idx].ues[ue_idx].port_eid[port_idx],
                          "port_eid", line_no);
}

static int parse_text_topo_file(const char *path, struct urma_topo_node **nodes, uint32_t *node_num)
{
    FILE *fp;
    char line[1024];
    int line_no = 0;
    int end_seen = 1;
    struct topo_text topo = {0};
    struct urma_topo_node *current = NULL;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *comment;
        char *tokens[8];
        int count;

        ++line_no;
        comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }

        count = split_line_tokens(line, tokens, 8);
        if (count < 0) {
            fprintf(stderr, "line %d: too many tokens\n", line_no);
            goto fail;
        }
        if (count == 0) {
            continue;
        }

        if (strcmp(tokens[0], "node") == 0) {
            if (!end_seen) {
                fprintf(stderr, "line %d: previous node missing endnode\n", line_no);
                goto fail;
            }
            if (parse_node_line(tokens, count, &topo, &current, line_no) != 0) {
                goto fail;
            }
            end_seen = 0;
        } else if (strcmp(tokens[0], "devcode") == 0) {
            if (parse_devcode_line(tokens, count, current, line_no) != 0) {
                goto fail;
            }
        } else if (strcmp(tokens[0], "ue") == 0) {
            if (parse_ue_line(tokens, count, current, line_no) != 0) {
                goto fail;
            }
        } else if (strcmp(tokens[0], "port_eid") == 0) {
            if (parse_port_eid_line(tokens, count, current, line_no) != 0) {
                goto fail;
            }
        } else if (strcmp(tokens[0], "endnode") == 0) {
            if (count != 1 || current == NULL || end_seen) {
                fprintf(stderr, "line %d: invalid endnode\n", line_no);
                goto fail;
            }
            current = NULL;
            end_seen = 1;
        } else {
            fprintf(stderr, "line %d: unknown directive %s\n", line_no, tokens[0]);
            goto fail;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "read %s failed: %s\n", path, strerror(errno));
        goto fail;
    }
    if (!end_seen) {
        fprintf(stderr, "file ended before endnode\n");
        goto fail;
    }
    if (topo.count == 0) {
        fprintf(stderr, "no topo nodes found in %s\n", path);
        goto fail;
    }
    if (*node_num != 0 && *node_num != topo.count) {
        fprintf(stderr, "node count mismatch: file=%u, expected=%u\n", topo.count, *node_num);
        goto fail;
    }

    fclose(fp);
    *nodes = topo.nodes;
    *node_num = topo.count;
    return 0;

fail:
    fclose(fp);
    free(topo.nodes);
    return -1;
}

static int parse_args(int argc, char **argv, struct tool_options *opts)
{
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--input") == 0) {
            if (++i >= argc) {
                return -1;
            }
            opts->input_path = argv[i];
        } else if (strcmp(argv[i], "--current-node") == 0) {
            if (++i >= argc || parse_u32(argv[i], &opts->current_node) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--count") == 0) {
            if (++i >= argc || parse_u32(argv[i], &opts->topo_num) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--print-only") == 0) {
            opts->print_only = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return -1;
        }
    }

    if (opts->input_path == NULL) {
        return -1;
    }
    if (opts->current_node == UINT32_MAX) {
        return -1;
    }

    return 0;
}

static void print_topology(const struct urma_topo_node *nodes, uint32_t topo_num)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t ue_idx;
    uint32_t port_idx;

    for (node_idx = 0; node_idx < topo_num; ++node_idx) {
        const struct urma_topo_node *node = &nodes[node_idx];
        printf("node[%u]: type=%u super_node_id=%u node_id=%u is_current=%u\n",
               node_idx, node->type, node->super_node_id, node->node_id, node->is_current);

        for (dev_idx = 0; dev_idx < DEV_NUM; ++dev_idx) {
            const struct urma_topo_agg_dev *dev = &node->agg_devs[dev_idx];
            char agg_eid[INET6_ADDRSTRLEN];
            int dev_has_content = 0;

            if (!is_zero_eid(dev->agg_eid)) {
                dev_has_content = 1;
            }
            for (ue_idx = 0; ue_idx < IODIE_NUM && !dev_has_content; ++ue_idx) {
                const struct urma_topo_ue *ue = &dev->ues[ue_idx];
                if (!is_zero_eid(ue->primary_eid) || ue->chip_id != 0 || ue->die_id != 0 || ue->entity_id != 0) {
                    dev_has_content = 1;
                    break;
                }
                for (port_idx = 0; port_idx < PORT_NUM; ++port_idx) {
                    if (!is_zero_eid(ue->port_eid[port_idx])) {
                        dev_has_content = 1;
                        break;
                    }
                }
            }
            if (!dev_has_content) {
                continue;
            }

            format_eid_ipv6(dev->agg_eid, agg_eid, sizeof(agg_eid));
            printf("  dev[%u]: agg_eid=%s\n", dev_idx, agg_eid);

            for (ue_idx = 0; ue_idx < IODIE_NUM; ++ue_idx) {
                const struct urma_topo_ue *ue = &dev->ues[ue_idx];
                char primary_eid[INET6_ADDRSTRLEN];
                int ue_has_content = 0;

                if (!is_zero_eid(ue->primary_eid) || ue->chip_id != 0 || ue->die_id != 0 || ue->entity_id != 0) {
                    ue_has_content = 1;
                }
                for (port_idx = 0; port_idx < PORT_NUM; ++port_idx) {
                    if (!is_zero_eid(ue->port_eid[port_idx])) {
                        ue_has_content = 1;
                        break;
                    }
                }
                if (!ue_has_content) {
                    continue;
                }

                format_eid_ipv6(ue->primary_eid, primary_eid, sizeof(primary_eid));
                printf("    ue[%u]: chip_id=%u die_id=%u entity_id=%u primary_eid=%s\n",
                       ue_idx, ue->chip_id, ue->die_id, ue->entity_id, primary_eid);

                for (port_idx = 0; port_idx < PORT_NUM; ++port_idx) {
                    char port_eid[INET6_ADDRSTRLEN];
                    if (is_zero_eid(ue->port_eid[port_idx])) {
                        continue;
                    }
                    format_eid_ipv6(ue->port_eid[port_idx], port_eid, sizeof(port_eid));
                    printf("      port_eid[%u]=%s\n", port_idx, port_eid);
                }
            }
        }
    }
}

static int ioctl_set_topo_ubcore(const char *dev_path, struct urma_topo_node *nodes, uint32_t topo_num)
{
    int fd;
    int ret;

    uvs_set_topo_t arg = {
        .in.topo_info = nodes,
        .in.topo_num = topo_num,
    };
    struct tpsa_cmd_hdr hdr = {
        .command = UVS_CMD_SET_TOPO,
        .args_len = (uint32_t)sizeof(arg),
        .args_addr = (uint64_t)(uintptr_t)&arg,
    };

    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open ubcore device %s failed: %s\n", dev_path, strerror(errno));
        return -1;
    }

    ret = ioctl(fd, TPSA_CMD, &hdr);
    if (ret != 0) {
        fprintf(stderr, "ubcore ioctl failed: ret=%d errno=%d(%s)\n", ret, errno, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    struct tool_options opts = {
        .current_node = UINT32_MAX,
    };
    struct urma_topo_node *nodes = NULL;
    int ret = 1;
    int current_found = 0;

    if (parse_args(argc, argv, &opts) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (parse_text_topo_file(opts.input_path, &nodes, &opts.topo_num) != 0) {
        return 1;
    }

    apply_current_node(nodes, opts.topo_num, opts.current_node, &current_found);
    if (!current_found) {
        fprintf(stderr, "current node id %u not found in topo\n", opts.current_node);
        goto out;
    }

    printf("parsed %u topo nodes from %s, node_size=%zu\n",
           opts.topo_num, opts.input_path, sizeof(struct urma_topo_node));

    if (opts.print_only) {
        print_topology(nodes, opts.topo_num);
        printf("print-only mode: ioctl skipped\n");
        ret = 0;
        goto out;
    }

    if (ioctl_set_topo_ubcore(UVS_UBCORE_DEVICE_PATH, nodes, opts.topo_num) != 0) {
        goto out;
    }
    printf("ubcore topo update succeeded\n");

    ret = 0;

out:
    free(nodes);
    return ret;
}
