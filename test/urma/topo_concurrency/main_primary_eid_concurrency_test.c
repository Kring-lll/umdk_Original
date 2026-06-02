/*
 * SPDX-License-Identifier: MIT
 *
 * Concurrent test for main_ue_eid table APIs.
 *
 * The test derives expected results from the old topo lookup semantics, then
 * verifies the current public lookup API under multi-threaded lookup pressure.
 * It also runs direct insert/update/delete/lookup API stress on disjoint
 * synthetic EIDs so topo-derived entries are not disturbed.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvs_api.h"

#define DEFAULT_LOOKUP_THREADS 8U
#define DEFAULT_LOOKUP_LOOP 32U
#define DEFAULT_API_THREADS 4U
#define DEFAULT_API_LOOP 8U
#define DEFAULT_API_ENTRY_NUM 64U
#define DEFAULT_WARN_EIDS 1024U
#define QUERY_PAIR_PREVIEW_MAX 16U
#define MULTI_PRIMARY_GROUP_PREVIEW_MAX 8U
#define MISMATCH_DETAIL_MAX 8U
#define API_EID_THREAD_MAX 0xffffU
#define API_EID_FIELD24_MAX 0xffffffU

/*
 * Old ubcore topo lookup collects at most ENTITY_AGG_DEV_NUM primary EIDs for
 * one node/entity/chip group before selecting the memcmp-min primary EID.
 */
#define OLD_TOPO_PRIMARY_LIMIT 3U

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

struct topo_group_key {
    uint32_t node_idx;
    uint32_t entity_id;
    uint32_t chip_id;
};

struct topo_group_candidate {
    struct topo_group_key key;
    uint32_t scan_seq;
    uvs_eid_t primary;
};

struct topo_group_record {
    struct topo_group_key key;
    uint32_t primary_num;
    uint32_t total_primary_num;
    uvs_eid_t primary[OLD_TOPO_PRIMARY_LIMIT];
    uvs_eid_t expected_main;
};

struct query_pair {
    uvs_eid_t eid;
    uvs_eid_t expected_main;
    struct topo_group_key key;
    bool is_port;
    uint32_t port_idx;
};

struct test_options {
    uint32_t lookup_threads;
    uint32_t lookup_loop;
    uint32_t api_threads;
    uint32_t api_loop;
    uint32_t api_entry_num;
    uint32_t require_eids;
    uint32_t warn_eids;
    uint32_t seed;
    bool flush_first;
    bool skip_set;
    bool check_only;
    bool shuffle;
    bool api_progress;
    bool require_multi_primary_group;
    const char *expected_topo_file;
};

struct lookup_thread_ctx {
    const struct query_pair *pairs;
    uint32_t pair_num;
    uint32_t thread_idx;
    uint32_t thread_num;
    uint32_t loop;
    uint64_t checked;
    int status;
};

struct api_thread_ctx {
    uint32_t thread_idx;
    uint32_t loop;
    uint32_t entry_num;
    bool progress;
    uint64_t checked;
    int status;
};

static pthread_mutex_t g_progress_lock = PTHREAD_MUTEX_INITIALIZER;

static bool raw_eid_valid(const char *raw)
{
    const char empty[EID_LEN] = {0};

    return raw != NULL && memcmp(raw, empty, EID_LEN) != 0;
}

static bool eid_equal(const uvs_eid_t *left, const uvs_eid_t *right)
{
    return memcmp(left->raw, right->raw, EID_LEN) == 0;
}

static void copy_raw_eid_to_uvs_eid(const char *raw, uvs_eid_t *eid)
{
    (void)memset(eid, 0, sizeof(*eid));
    (void)memcpy(eid->raw, raw, EID_LEN);
}

static void eid_to_str(const uvs_eid_t *eid, char *buf, size_t len)
{
    if (inet_ntop(AF_INET6, eid->raw, buf, len) == NULL) {
        (void)snprintf(buf, len, "<invalid>");
    }
}

static void print_eid(const char *label, const uvs_eid_t *eid)
{
    char buf[INET6_ADDRSTRLEN] = {0};

    eid_to_str(eid, buf, sizeof(buf));
    printf("%s%s\n", label, buf);
}

static uint32_t prng_next(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static int parse_u32_arg(const char *name, const char *value, uint32_t *out)
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

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --lookup-threads N  topo consistency lookup threads, default %u\n",
           DEFAULT_LOOKUP_THREADS);
    printf("  --lookup-loop N     lookup passes per thread, default %u\n",
           DEFAULT_LOOKUP_LOOP);
    printf("  --api-threads N     direct API stress threads, default %u; 0 disables it\n",
           DEFAULT_API_THREADS);
    printf("  --api-loop N        direct API stress rounds per thread, default %u\n",
           DEFAULT_API_LOOP);
    printf("  --api-entries N     synthetic entries per API thread round, default %u\n",
           DEFAULT_API_ENTRY_NUM);
    printf("  --api-progress      print direct API stress phase progress\n");
    printf("  --flush-first       flush main_ue_eid table before set_topo refresh\n");
    printf("  --skip-set          do not refresh table through uvs_set_topo_info\n");
    printf("  --check-only        only run sequential old/new consistency check\n");
    printf("  --require-eids N    fail if topo-derived query EIDs are fewer than N\n");
    printf("  --warn-eids N       warn if topo-derived query EIDs are fewer than N, default %u\n",
           DEFAULT_WARN_EIDS);
    printf("  --require-multi-primary-group\n");
    printf("                      fail unless one node/entity/chip group has multiple primary EIDs\n");
    printf("  --shuffle           shuffle topo-derived query pairs before threaded lookup\n");
    printf("  --seed N            deterministic shuffle seed, default 1\n");
    printf("  --expected-topo-file FILE\n");
    printf("                      derive expected lookup pairs from dumped topo file\n");
}

static int parse_args(int argc, char **argv, struct test_options *opts)
{
    int i;

    opts->lookup_threads = DEFAULT_LOOKUP_THREADS;
    opts->lookup_loop = DEFAULT_LOOKUP_LOOP;
    opts->api_threads = DEFAULT_API_THREADS;
    opts->api_loop = DEFAULT_API_LOOP;
    opts->api_entry_num = DEFAULT_API_ENTRY_NUM;
    opts->require_eids = 0;
    opts->warn_eids = DEFAULT_WARN_EIDS;
    opts->seed = 1;
    opts->flush_first = false;
    opts->skip_set = false;
    opts->check_only = false;
    opts->shuffle = false;
    opts->api_progress = false;
    opts->require_multi_primary_group = false;
    opts->expected_topo_file = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lookup-threads") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--lookup-threads", argv[++i],
                              &opts->lookup_threads) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--lookup-loop") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--lookup-loop", argv[++i],
                              &opts->lookup_loop) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--api-threads") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--api-threads", argv[++i],
                              &opts->api_threads) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--api-loop") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--api-loop", argv[++i],
                              &opts->api_loop) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--api-entries") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--api-entries", argv[++i],
                              &opts->api_entry_num) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--require-eids") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--require-eids", argv[++i],
                              &opts->require_eids) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--warn-eids") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--warn-eids", argv[++i],
                              &opts->warn_eids) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--seed", argv[++i], &opts->seed) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--expected-topo-file") == 0 &&
                   i + 1 < argc) {
            opts->expected_topo_file = argv[++i];
        } else if (strcmp(argv[i], "--flush-first") == 0) {
            opts->flush_first = true;
        } else if (strcmp(argv[i], "--skip-set") == 0) {
            opts->skip_set = true;
        } else if (strcmp(argv[i], "--check-only") == 0) {
            opts->check_only = true;
        } else if (strcmp(argv[i], "--shuffle") == 0) {
            opts->shuffle = true;
        } else if (strcmp(argv[i], "--api-progress") == 0) {
            opts->api_progress = true;
        } else if (strcmp(argv[i], "--require-multi-primary-group") == 0) {
            opts->require_multi_primary_group = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return -EINVAL;
        }
    }

    if (opts->lookup_threads == 0 || opts->lookup_loop == 0 ||
        opts->api_loop == 0 || opts->api_entry_num == 0) {
        fprintf(stderr, "thread/loop/entry options out of range\n");
        return -EINVAL;
    }
    if (opts->api_threads > API_EID_THREAD_MAX ||
        opts->api_loop > API_EID_FIELD24_MAX ||
        opts->api_entry_num > API_EID_FIELD24_MAX) {
        fprintf(stderr, "api thread/loop/entry options exceed synthetic EID encoding range\n");
        return -EINVAL;
    }

    return 0;
}

static int load_current_topo(struct topo_map_debug **topo_out)
{
    struct topo_map_debug *topo;
    int ret;

    topo = calloc(1, sizeof(*topo));
    if (topo == NULL) {
        return -ENOMEM;
    }

    ret = uvs_get_topo_info(topo);
    if (ret != 0) {
        fprintf(stderr, "uvs_get_topo_info failed, ret=%d\n", ret);
        free(topo);
        return ret;
    }

    if (topo->node_num == 0 || topo->node_num > MAX_NODE_NUM) {
        fprintf(stderr, "invalid topo node_num=%u\n", topo->node_num);
        free(topo);
        return -EINVAL;
    }

    *topo_out = topo;
    return 0;
}

static int load_expected_topo_file(const char *path,
                                   struct topo_map_debug **topo_out)
{
    struct expected_topo_file_header header;
    struct topo_map_debug *topo;
    FILE *fp;
    size_t count;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open expected topo file %s: %s\n",
                path, strerror(errno));
        return -errno;
    }

    count = fread(&header, sizeof(header), 1, fp);
    if (count != 1) {
        int err = ferror(fp) ? errno : EIO;

        (void)fclose(fp);
        fprintf(stderr, "failed to read expected topo header %s: %s\n",
                path, strerror(err));
        return -err;
    }

    if (memcmp(header.magic, EXPECTED_TOPO_FILE_MAGIC,
               sizeof(header.magic)) != 0 ||
        header.version != EXPECTED_TOPO_FILE_VERSION ||
        header.node_size != sizeof(struct urma_topo_node) ||
        header.node_num == 0 || header.node_num > MAX_NODE_NUM) {
        (void)fclose(fp);
        fprintf(stderr, "invalid expected topo file %s\n", path);
        return -EINVAL;
    }

    topo = calloc(1, sizeof(*topo));
    if (topo == NULL) {
        (void)fclose(fp);
        return -ENOMEM;
    }

    count = fread(topo->topo_infos, sizeof(topo->topo_infos[0]),
                  header.node_num, fp);
    if (count != header.node_num) {
        int err = ferror(fp) ? errno : EIO;

        free(topo);
        (void)fclose(fp);
        fprintf(stderr, "failed to read expected topo body %s: %s\n",
                path, strerror(err));
        return -err;
    }

    if (fclose(fp) != 0) {
        int err = errno;

        free(topo);
        fprintf(stderr, "failed to close expected topo file %s: %s\n",
                path, strerror(err));
        return -err;
    }

    topo->node_num = header.node_num;
    *topo_out = topo;
    return 0;
}

static int refresh_table_by_topo(const struct topo_map_debug *topo)
{
    int ret;

    printf("refresh lookup table by current topo: node_num=%u node_size=%zu total_size=%zu bytes\n",
           topo->node_num, sizeof(struct urma_topo_node),
           (size_t)topo->node_num * sizeof(struct urma_topo_node));

    ret = uvs_set_topo_info((void *)topo->topo_infos,
                            sizeof(struct urma_topo_node), topo->node_num);
    if (ret != 0) {
        fprintf(stderr, "uvs_set_topo_info failed, ret=%d\n", ret);
        return ret;
    }

    return 0;
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

static uint32_t count_valid_primary_ues(const struct topo_map_debug *topo)
{
    uint32_t node_idx;
    uint32_t total = 0;

    for (node_idx = 0; node_idx < topo->node_num; node_idx++) {
        const struct urma_topo_node *node = &topo->topo_infos[node_idx];
        uint32_t dev_idx;

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];
            uint32_t die_idx;

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                if (raw_eid_valid(agg_dev->ues[die_idx].primary_eid)) {
                    total++;
                }
            }
        }
    }

    return total;
}

static uint64_t count_query_pairs_from_topo(const struct topo_map_debug *topo)
{
    uint64_t total = 0;
    uint32_t node_idx;

    for (node_idx = 0; node_idx < topo->node_num; node_idx++) {
        const struct urma_topo_node *node = &topo->topo_infos[node_idx];
        uint32_t dev_idx;

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];
            uint32_t die_idx;

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                const struct urma_topo_ue *ue = &agg_dev->ues[die_idx];

                if (!raw_eid_valid(ue->primary_eid)) {
                    continue;
                }

                total++;
                total += count_valid_ports(ue);
            }
        }
    }

    return total;
}

static int compare_key(const struct topo_group_key *left,
                       const struct topo_group_key *right)
{
    if (left->node_idx != right->node_idx) {
        return left->node_idx < right->node_idx ? -1 : 1;
    }
    if (left->entity_id != right->entity_id) {
        return left->entity_id < right->entity_id ? -1 : 1;
    }
    if (left->chip_id != right->chip_id) {
        return left->chip_id < right->chip_id ? -1 : 1;
    }
    return 0;
}

static int compare_group_candidate(const void *left, const void *right)
{
    const struct topo_group_candidate *l = left;
    const struct topo_group_candidate *r = right;
    int ret = compare_key(&l->key, &r->key);

    if (ret != 0) {
        return ret;
    }
    if (l->scan_seq != r->scan_seq) {
        return l->scan_seq < r->scan_seq ? -1 : 1;
    }
    return 0;
}

static int compare_group_record_by_key(const void *key, const void *record)
{
    const struct topo_group_key *k = key;
    const struct topo_group_record *r = record;

    return compare_key(k, &r->key);
}

static void collect_group_candidates(const struct topo_map_debug *topo,
                                     struct topo_group_candidate *candidates)
{
    uint32_t node_idx;
    uint32_t candidate_num = 0;
    uint32_t scan_seq = 0;

    for (node_idx = 0; node_idx < topo->node_num; node_idx++) {
        const struct urma_topo_node *node = &topo->topo_infos[node_idx];
        uint32_t dev_idx;

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];
            uint32_t die_idx;

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                const struct urma_topo_ue *ue = &agg_dev->ues[die_idx];

                if (!raw_eid_valid(ue->primary_eid)) {
                    continue;
                }

                candidates[candidate_num].key.node_idx = node_idx;
                candidates[candidate_num].key.entity_id = ue->entity_id;
                candidates[candidate_num].key.chip_id = ue->chip_id;
                candidates[candidate_num].scan_seq = scan_seq;
                copy_raw_eid_to_uvs_eid(ue->primary_eid,
                                        &candidates[candidate_num].primary);
                candidate_num++;
                scan_seq++;
            }
        }
    }
}

static void update_group_expected(struct topo_group_record *record)
{
    uint32_t i;

    record->expected_main = record->primary[0];
    for (i = 1; i < record->primary_num; i++) {
        if (memcmp(record->expected_main.raw, record->primary[i].raw,
                   EID_LEN) > 0) {
            record->expected_main = record->primary[i];
        }
    }
}

static int build_group_records(struct topo_group_candidate *candidates,
                               uint32_t candidate_num,
                               struct topo_group_record **records_out,
                               uint32_t *record_num_out)
{
    struct topo_group_record *records;
    uint32_t record_num = 0;
    uint32_t i;

    if (candidate_num == 0) {
        fprintf(stderr, "topo has no valid primary EIDs\n");
        return -ENOENT;
    }

    records = calloc(candidate_num, sizeof(*records));
    if (records == NULL) {
        return -ENOMEM;
    }

    qsort(candidates, candidate_num, sizeof(*candidates),
          compare_group_candidate);

    for (i = 0; i < candidate_num; i++) {
        struct topo_group_record *record;

        if (record_num == 0 ||
            compare_key(&records[record_num - 1].key,
                        &candidates[i].key) != 0) {
            record = &records[record_num++];
            record->key = candidates[i].key;
        } else {
            record = &records[record_num - 1];
        }

        if (record->primary_num < OLD_TOPO_PRIMARY_LIMIT) {
            record->primary[record->primary_num++] = candidates[i].primary;
        }
        record->total_primary_num++;
    }

    for (i = 0; i < record_num; i++) {
        if (records[i].primary_num == 0) {
            free(records);
            return -EINVAL;
        }
        update_group_expected(&records[i]);
    }

    *records_out = records;
    *record_num_out = record_num;
    return 0;
}

static const struct topo_group_record *find_group_record(
    const struct topo_group_record *records, uint32_t record_num,
    const struct topo_group_key *key)
{
    return bsearch(key, records, record_num, sizeof(*records),
                   compare_group_record_by_key);
}

static int add_query_pair(struct query_pair *pairs, uint32_t *pair_num,
                          const char *raw_eid,
                          const struct topo_group_record *record,
                          bool is_port, uint32_t port_idx)
{
    struct query_pair *pair = &pairs[*pair_num];

    copy_raw_eid_to_uvs_eid(raw_eid, &pair->eid);
    pair->expected_main = record->expected_main;
    pair->key = record->key;
    pair->is_port = is_port;
    pair->port_idx = port_idx;
    (*pair_num)++;
    return 0;
}

static int collect_query_pairs(const struct topo_map_debug *topo,
                               const struct topo_group_record *records,
                               uint32_t record_num,
                               struct query_pair **pairs_out,
                               uint32_t *pair_num_out)
{
    struct query_pair *pairs;
    uint64_t total;
    uint32_t pair_num = 0;
    uint32_t node_idx;

    total = count_query_pairs_from_topo(topo);
    if (total == 0 || total > UINT32_MAX) {
        fprintf(stderr, "invalid topo query pair count=%" PRIu64 "\n", total);
        return total == 0 ? -ENOENT : -E2BIG;
    }
    if (total > SIZE_MAX / sizeof(*pairs)) {
        return -EOVERFLOW;
    }

    pairs = calloc((size_t)total, sizeof(*pairs));
    if (pairs == NULL) {
        return -ENOMEM;
    }

    for (node_idx = 0; node_idx < topo->node_num; node_idx++) {
        const struct urma_topo_node *node = &topo->topo_infos[node_idx];
        uint32_t dev_idx;

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *agg_dev = &node->agg_devs[dev_idx];
            uint32_t die_idx;

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                const struct urma_topo_ue *ue = &agg_dev->ues[die_idx];
                struct topo_group_key key;
                const struct topo_group_record *record;
                uint32_t port_idx;

                if (!raw_eid_valid(ue->primary_eid)) {
                    continue;
                }

                key.node_idx = node_idx;
                key.entity_id = ue->entity_id;
                key.chip_id = ue->chip_id;
                record = find_group_record(records, record_num, &key);
                if (record == NULL) {
                    free(pairs);
                    return -ENOENT;
                }

                (void)add_query_pair(pairs, &pair_num, ue->primary_eid,
                                     record, false, 0);
                for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
                    if (!raw_eid_valid(ue->port_eid[port_idx])) {
                        continue;
                    }
                    (void)add_query_pair(pairs, &pair_num,
                                         ue->port_eid[port_idx],
                                         record, true, port_idx);
                }
            }
        }
    }

    if (pair_num != (uint32_t)total) {
        fprintf(stderr, "topo query pair count changed, count=%u expected=%u\n",
                pair_num, (uint32_t)total);
        free(pairs);
        return -EAGAIN;
    }

    *pairs_out = pairs;
    *pair_num_out = pair_num;
    return 0;
}

static int build_expected_pairs_from_topo(const struct topo_map_debug *topo,
                                          struct topo_group_record **records_out,
                                          uint32_t *record_num_out,
                                          struct query_pair **pairs_out,
                                          uint32_t *pair_num_out)
{
    struct topo_group_candidate *candidates = NULL;
    struct topo_group_record *records = NULL;
    struct query_pair *pairs = NULL;
    uint32_t candidate_num;
    uint32_t record_num = 0;
    uint32_t pair_num = 0;
    int ret;

    candidate_num = count_valid_primary_ues(topo);
    if (candidate_num == 0) {
        return -ENOENT;
    }
    candidates = calloc(candidate_num, sizeof(*candidates));
    if (candidates == NULL) {
        return -ENOMEM;
    }

    collect_group_candidates(topo, candidates);
    ret = build_group_records(candidates, candidate_num,
                              &records, &record_num);
    free(candidates);
    if (ret != 0) {
        return ret;
    }

    ret = collect_query_pairs(topo, records, record_num, &pairs, &pair_num);
    if (ret != 0) {
        free(records);
        return ret;
    }

    *records_out = records;
    *record_num_out = record_num;
    *pairs_out = pairs;
    *pair_num_out = pair_num;
    return 0;
}

static void shuffle_query_pairs(struct query_pair *pairs, uint32_t pair_num,
                                uint32_t seed)
{
    uint32_t state = seed == 0 ? 1U : seed;
    uint32_t i;

    if (pairs == NULL || pair_num <= 1) {
        return;
    }

    for (i = pair_num - 1; i > 0; i--) {
        uint32_t j = prng_next(&state) % (i + 1);
        struct query_pair tmp = pairs[i];

        pairs[i] = pairs[j];
        pairs[j] = tmp;
    }
}

static void print_group_summary(const struct topo_group_record *records,
                                uint32_t record_num)
{
    uint32_t truncated_group_num = 0;
    uint32_t multi_primary_group_num = 0;
    uint32_t i;

    for (i = 0; i < record_num; i++) {
        if (records[i].total_primary_num > 1) {
            multi_primary_group_num++;
        }
        if (records[i].total_primary_num > records[i].primary_num) {
            truncated_group_num++;
        }
    }

    printf("old-topo expected groups=%u, multi-primary groups=%u, "
           "groups over old limit(%u)=%u\n",
           record_num, multi_primary_group_num, OLD_TOPO_PRIMARY_LIMIT,
           truncated_group_num);
    if (truncated_group_num != 0) {
        printf("note: old topo lookup only considers the first %u primary EIDs "
               "in dev/die scan order for each node/entity/chip group\n",
               OLD_TOPO_PRIMARY_LIMIT);
    }
}

static uint32_t count_multi_primary_groups(const struct topo_group_record *records,
                                           uint32_t record_num)
{
    uint32_t multi_primary_group_num = 0;
    uint32_t i;

    for (i = 0; i < record_num; i++) {
        if (records[i].total_primary_num > 1) {
            multi_primary_group_num++;
        }
    }

    return multi_primary_group_num;
}

static void print_multi_primary_group_preview(
    const struct topo_group_record *records, uint32_t record_num)
{
    uint32_t printed_num = 0;
    uint32_t i;

    printf("\nmulti-primary group preview: max=%u\n",
           MULTI_PRIMARY_GROUP_PREVIEW_MAX);
    for (i = 0; i < record_num; i++) {
        const struct topo_group_record *record = &records[i];
        char expected_buf[INET6_ADDRSTRLEN] = {0};
        uint32_t primary_idx;

        if (record->total_primary_num <= 1) {
            continue;
        }
        if (printed_num >= MULTI_PRIMARY_GROUP_PREVIEW_MAX) {
            break;
        }

        eid_to_str(&record->expected_main, expected_buf,
                   sizeof(expected_buf));
        printf("multi_group[%u] node_idx=%u entity=%u chip=%u "
               "primary_num=%u stored=%u expected_main=%s\n",
               printed_num, record->key.node_idx, record->key.entity_id,
               record->key.chip_id, record->total_primary_num,
               record->primary_num, expected_buf);
        for (primary_idx = 0; primary_idx < record->primary_num;
             primary_idx++) {
            char primary_buf[INET6_ADDRSTRLEN] = {0};

            eid_to_str(&record->primary[primary_idx], primary_buf,
                       sizeof(primary_buf));
            printf("  primary[%u]=%s\n", primary_idx, primary_buf);
        }
        if (record->total_primary_num > record->primary_num) {
            printf("  ... %u more primary EIDs ignored by old-topo rule\n",
                   record->total_primary_num - record->primary_num);
        }
        printed_num++;
    }
    if (printed_num == 0) {
        printf("no multi-primary node/entity/chip group found\n");
    }
}

static void print_pair_preview(const struct query_pair *pairs, uint32_t pair_num)
{
    uint32_t preview_num = pair_num < QUERY_PAIR_PREVIEW_MAX ?
                           pair_num : QUERY_PAIR_PREVIEW_MAX;
    uint32_t i;

    printf("\nquery preview: total=%u preview=%u\n", pair_num, preview_num);
    for (i = 0; i < preview_num; i++) {
        char input_buf[INET6_ADDRSTRLEN] = {0};
        char expected_buf[INET6_ADDRSTRLEN] = {0};
        const char *port_str = pairs[i].is_port ? "port" : "primary";
        char port_buf[16] = {0};

        eid_to_str(&pairs[i].eid, input_buf, sizeof(input_buf));
        eid_to_str(&pairs[i].expected_main, expected_buf,
                   sizeof(expected_buf));
        if (pairs[i].is_port) {
            (void)snprintf(port_buf, sizeof(port_buf), "%u",
                           pairs[i].port_idx);
        } else {
            (void)snprintf(port_buf, sizeof(port_buf), "-");
        }
        printf("pair[%u] type=%s port=%s node_idx=%u entity=%u chip=%u "
               "input=%s expected_main=%s\n",
               i, port_str, port_buf,
               pairs[i].key.node_idx, pairs[i].key.entity_id,
               pairs[i].key.chip_id, input_buf, expected_buf);
    }
    if (pair_num > preview_num) {
        printf("... %u more query pairs omitted\n", pair_num - preview_num);
    }
}

static int lookup_one_pair(const struct query_pair *pair, uvs_eid_t *actual_out)
{
    uvs_eid_t actual = {0};
    int ret;

    ret = uvs_lookup_main_ue_eid(&pair->eid, &actual);
    if (ret != 0) {
        return ret;
    }
    if (!eid_equal(&actual, &pair->expected_main)) {
        if (actual_out != NULL) {
            *actual_out = actual;
        }
        return -EBADMSG;
    }
    if (actual_out != NULL) {
        *actual_out = actual;
    }
    return 0;
}

static void print_lookup_mismatch(const char *phase, const struct query_pair *pair,
                                  const uvs_eid_t *actual, int ret)
{
    char input_buf[INET6_ADDRSTRLEN] = {0};
    char expected_buf[INET6_ADDRSTRLEN] = {0};
    char actual_buf[INET6_ADDRSTRLEN] = {0};
    char port_buf[16] = {0};

    eid_to_str(&pair->eid, input_buf, sizeof(input_buf));
    eid_to_str(&pair->expected_main, expected_buf, sizeof(expected_buf));
    if (actual != NULL) {
        eid_to_str(actual, actual_buf, sizeof(actual_buf));
    } else {
        (void)snprintf(actual_buf, sizeof(actual_buf), "<ret %d>", ret);
    }
    if (pair->is_port) {
        (void)snprintf(port_buf, sizeof(port_buf), "%u", pair->port_idx);
    } else {
        (void)snprintf(port_buf, sizeof(port_buf), "-");
    }

    fprintf(stderr, "%s node_idx=%u entity=%u chip=%u type=%s port=%s "
            "input=%s expected=%s actual=%s ret=%d\n",
            phase, pair->key.node_idx, pair->key.entity_id,
            pair->key.chip_id, pair->is_port ? "port" : "primary",
            port_buf, input_buf,
            expected_buf, actual_buf, ret);
}

static int validate_multi_primary_group_pairs(
    const struct topo_group_record *records, uint32_t record_num,
    const struct query_pair *pairs, uint32_t pair_num, bool require_group)
{
    uint32_t multi_primary_group_num;
    uint32_t detail_num = 0;
    uint32_t checked_num = 0;
    uint32_t i;

    multi_primary_group_num = count_multi_primary_groups(records, record_num);
    print_multi_primary_group_preview(records, record_num);
    if (multi_primary_group_num == 0) {
        if (require_group) {
            fprintf(stderr, "no multi-primary node/entity/chip group found\n");
            return -ENOENT;
        }
        printf("skip multi-primary consistency: no matching group\n");
        return 0;
    }

    for (i = 0; i < pair_num; i++) {
        const struct topo_group_record *record;
        uvs_eid_t actual = {0};
        int ret;

        record = find_group_record(records, record_num, &pairs[i].key);
        if (record == NULL) {
            return -ENOENT;
        }
        if (record->total_primary_num <= 1) {
            continue;
        }

        ret = lookup_one_pair(&pairs[i], &actual);
        checked_num++;
        if (ret == 0) {
            continue;
        }

        if (detail_num < MISMATCH_DETAIL_MAX) {
            print_lookup_mismatch("multi-primary-group", &pairs[i],
                                  ret == -EBADMSG ? &actual : NULL, ret);
            detail_num++;
        }
    }

    if (detail_num != 0) {
        fprintf(stderr, "multi-primary-group consistency failed, "
                "groups=%u checked=%u detail_printed=%u\n",
                multi_primary_group_num, checked_num, detail_num);
        return -EINVAL;
    }

    printf("multi-primary-group consistency passed: groups=%u checked=%u, "
           "mismatch=0\n", multi_primary_group_num, checked_num);
    return 0;
}

static int validate_all_pairs(const struct query_pair *pairs, uint32_t pair_num,
                              const char *phase)
{
    uint32_t detail_num = 0;
    uint32_t i;

    for (i = 0; i < pair_num; i++) {
        uvs_eid_t actual = {0};
        int ret = lookup_one_pair(&pairs[i], &actual);

        if (ret == 0) {
            continue;
        }

        if (detail_num < MISMATCH_DETAIL_MAX) {
            print_lookup_mismatch(phase, &pairs[i],
                                  ret == -EBADMSG ? &actual : NULL, ret);
            detail_num++;
        }
    }

    if (detail_num != 0) {
        fprintf(stderr, "%s consistency failed, checked=%u detail_printed=%u\n",
                phase, pair_num, detail_num);
        return -EINVAL;
    }

    printf("%s consistency passed: checked=%u, mismatch=0\n", phase, pair_num);
    return 0;
}

static void print_actual_lookup_preview(const struct query_pair *pairs,
                                        uint32_t pair_num)
{
    uint32_t preview_num = pair_num < QUERY_PAIR_PREVIEW_MAX ?
                           pair_num : QUERY_PAIR_PREVIEW_MAX;
    uint32_t i;

    printf("\nactual lookup preview: %u sample(s)\n", preview_num);
    for (i = 0; i < preview_num; i++) {
        uvs_eid_t actual = {0};
        char input_buf[INET6_ADDRSTRLEN] = {0};
        char expected_buf[INET6_ADDRSTRLEN] = {0};
        char actual_buf[INET6_ADDRSTRLEN] = {0};
        int ret;

        eid_to_str(&pairs[i].eid, input_buf, sizeof(input_buf));
        eid_to_str(&pairs[i].expected_main, expected_buf,
                   sizeof(expected_buf));
        ret = uvs_lookup_main_ue_eid(&pairs[i].eid, &actual);
        if (ret == 0) {
            eid_to_str(&actual, actual_buf, sizeof(actual_buf));
        } else {
            (void)snprintf(actual_buf, sizeof(actual_buf), "<ret %d>", ret);
        }

        printf("lookup[%u] input=%s expected=%s actual=%s match=%s\n",
               i, input_buf, expected_buf, actual_buf,
               ret == 0 && eid_equal(&actual, &pairs[i].expected_main) ?
               "yes" : "no");
    }
}

static void *lookup_thread_main(void *arg)
{
    struct lookup_thread_ctx *ctx = arg;
    uint32_t round;

    ctx->status = 0;
    ctx->checked = 0;

    for (round = 0; round < ctx->loop; round++) {
        uint32_t i;

        for (i = ctx->thread_idx; i < ctx->pair_num; i += ctx->thread_num) {
            uvs_eid_t actual = {0};
            int ret = lookup_one_pair(&ctx->pairs[i], &actual);

            ctx->checked++;
            if (ret != 0) {
                print_lookup_mismatch("threaded", &ctx->pairs[i],
                                      ret == -EBADMSG ? &actual : NULL, ret);
                ctx->status = ret;
                return NULL;
            }
        }
    }

    return NULL;
}

static int run_concurrent_lookup(const struct query_pair *pairs, uint32_t pair_num,
                                 uint32_t thread_num, uint32_t loop)
{
    pthread_t *threads;
    struct lookup_thread_ctx *ctxs;
    uint64_t checked = 0;
    uint32_t i;
    int ret = 0;

    threads = calloc(thread_num, sizeof(*threads));
    ctxs = calloc(thread_num, sizeof(*ctxs));
    if (threads == NULL || ctxs == NULL) {
        free(threads);
        free(ctxs);
        return -ENOMEM;
    }

    printf("\nstart threaded lookup consistency: threads=%u loop=%u pairs=%u\n",
           thread_num, loop, pair_num);
    for (i = 0; i < thread_num; i++) {
        int err;

        ctxs[i].pairs = pairs;
        ctxs[i].pair_num = pair_num;
        ctxs[i].thread_idx = i;
        ctxs[i].thread_num = thread_num;
        ctxs[i].loop = loop;
        err = pthread_create(&threads[i], NULL, lookup_thread_main,
                             &ctxs[i]);
        if (err != 0) {
            ret = -err;
            thread_num = i;
            break;
        }
    }

    for (i = 0; i < thread_num; i++) {
        (void)pthread_join(threads[i], NULL);
        checked += ctxs[i].checked;
        if (ctxs[i].status != 0 && ret == 0) {
            ret = ctxs[i].status;
        }
    }

    free(ctxs);
    free(threads);

    if (ret != 0) {
        fprintf(stderr, "threaded lookup consistency failed, checked=%" PRIu64
                ", ret=%d\n", checked, ret);
        return ret;
    }

    printf("threaded lookup consistency passed: checked=%" PRIu64
           ", mismatch=0\n", checked);
    return 0;
}

static void make_api_eid(uint32_t thread_idx, uint32_t round, uint32_t entry_idx,
                         uint8_t kind, uvs_eid_t *eid)
{
    (void)memset(eid, 0, sizeof(*eid));
    eid->raw[0] = 0x20;
    eid->raw[1] = 0x01;
    eid->raw[2] = 0x0d;
    eid->raw[3] = 0xb8;
    eid->raw[4] = 0xfa;
    eid->raw[5] = 0xce;
    eid->raw[6] = kind;
    eid->raw[7] = (uint8_t)(thread_idx >> 8);
    eid->raw[8] = (uint8_t)thread_idx;
    eid->raw[9] = (uint8_t)(round >> 16);
    eid->raw[10] = (uint8_t)(round >> 8);
    eid->raw[11] = (uint8_t)round;
    eid->raw[12] = (uint8_t)(entry_idx >> 16);
    eid->raw[13] = (uint8_t)(entry_idx >> 8);
    eid->raw[14] = (uint8_t)entry_idx;
    eid->raw[15] = kind;
}

static void fill_api_entries(uvs_main_ue_eid_entry_t *entries,
                             uint32_t entry_num, uint32_t thread_idx,
                             uint32_t round, const uvs_eid_t *main_ue)
{
    uint32_t i;

    for (i = 0; i < entry_num; i++) {
        make_api_eid(thread_idx, round, i, 0x11, &entries[i].eid);
        entries[i].main_ue_eid = *main_ue;
    }
}

static int verify_api_lookup(const uvs_main_ue_eid_entry_t *entries,
                             uint32_t entry_num, const uvs_eid_t *expected,
                             bool expect_missing)
{
    uint32_t i;

    for (i = 0; i < entry_num; i++) {
        uvs_eid_t actual = {0};
        int ret = uvs_lookup_main_ue_eid(&entries[i].eid, &actual);

        if (expect_missing) {
            if (ret == -ENOENT) {
                continue;
            }
            fprintf(stderr, "synthetic lookup missing check failed, ret=%d\n",
                    ret);
            print_eid("  eid=", &entries[i].eid);
            return ret == 0 ? -EEXIST : ret;
        }

        if (ret != 0 || !eid_equal(&actual, expected)) {
            fprintf(stderr, "synthetic lookup mismatch, ret=%d\n", ret);
            print_eid("  eid=", &entries[i].eid);
            print_eid("  expected=", expected);
            if (ret == 0) {
                print_eid("  actual=", &actual);
            }
            return ret == 0 ? -EBADMSG : ret;
        }
    }

    return 0;
}

static int delete_api_entries(const uvs_main_ue_eid_entry_t *entries,
                              uint32_t entry_num)
{
    uint32_t i;

    for (i = 0; i < entry_num; i++) {
        int ret = uvs_delete_main_ue_eid(&entries[i].eid);
        if (ret != 0) {
            fprintf(stderr, "synthetic delete failed, ret=%d index=%u\n",
                    ret, i);
            print_eid("  eid=", &entries[i].eid);
            return ret;
        }
    }

    return 0;
}

static void api_progress(bool enabled, uint32_t thread_idx, uint32_t round,
                         const char *phase);

static int insert_api_entries(uvs_main_ue_eid_entry_t *entries,
                              uint32_t entry_num)
{
    uint32_t i;
    int ret;

    for (i = 0; i < entry_num; i++) {
        ret = uvs_insert_main_ue_eid(&entries[i]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int reinsert_even_entries_by_single_loop(uvs_main_ue_eid_entry_t *entries,
                                                uint32_t entry_num,
                                                const uvs_eid_t *main_ue,
                                                uint32_t thread_idx,
                                                uint32_t round, bool progress)
{
    uint32_t i;
    int ret;

    api_progress(progress, thread_idx, round, "single-reinsert-even begin");
    for (i = 0; i < entry_num; i += 2) {
        entries[i].main_ue_eid = *main_ue;
        ret = uvs_insert_main_ue_eid(&entries[i]);
        if (ret != 0) {
            api_progress(progress, thread_idx, round,
                         "single-reinsert-even failed");
            return ret;
        }
    }
    api_progress(progress, thread_idx, round, "single-reinsert-even end");
    return 0;
}

static void api_progress(bool enabled, uint32_t thread_idx, uint32_t round,
                         const char *phase)
{
    if (!enabled) {
        return;
    }

    (void)pthread_mutex_lock(&g_progress_lock);
    printf("api-progress thread=%u round=%u %s\n", thread_idx, round, phase);
    fflush(stdout);
    (void)pthread_mutex_unlock(&g_progress_lock);
}

static int run_api_round(uint32_t thread_idx, uint32_t round,
                         uvs_main_ue_eid_entry_t *entries,
                         uint32_t entry_num, bool progress)
{
    uvs_eid_t main_a = {0};
    uvs_eid_t main_b = {0};
    uvs_eid_t main_c = {0};
    uint32_t i;
    int ret;

    make_api_eid(thread_idx, round, 0, 0xa1, &main_a);
    make_api_eid(thread_idx, round, 0, 0xb2, &main_b);
    make_api_eid(thread_idx, round, 0, 0xc3, &main_c);

    fill_api_entries(entries, entry_num, thread_idx, round, &main_a);
    api_progress(progress, thread_idx, round, "insert-all begin");
    ret = insert_api_entries(entries, entry_num);
    if (ret != 0) {
        fprintf(stderr, "synthetic insert failed, ret=%d\n", ret);
        return ret;
    }
    api_progress(progress, thread_idx, round, "insert-all end");
    api_progress(progress, thread_idx, round, "lookup-main-a begin");
    ret = verify_api_lookup(entries, entry_num, &main_a, false);
    if (ret != 0) {
        return ret;
    }
    api_progress(progress, thread_idx, round, "lookup-main-a end");

    for (i = 0; i < entry_num; i++) {
        entries[i].main_ue_eid = main_b;
    }
    api_progress(progress, thread_idx, round, "update-all begin");
    ret = insert_api_entries(entries, entry_num);
    if (ret != 0) {
        fprintf(stderr, "synthetic update failed, ret=%d\n", ret);
        return ret;
    }
    api_progress(progress, thread_idx, round, "update-all end");
    api_progress(progress, thread_idx, round, "lookup-main-b begin");
    ret = verify_api_lookup(entries, entry_num, &main_b, false);
    if (ret != 0) {
        return ret;
    }
    api_progress(progress, thread_idx, round, "lookup-main-b end");

    api_progress(progress, thread_idx, round, "delete-even begin");
    for (i = 0; i < entry_num; i += 2) {
        ret = uvs_delete_main_ue_eid(&entries[i].eid);
        if (ret != 0) {
            fprintf(stderr, "synthetic even delete failed, ret=%d index=%u\n",
                    ret, i);
            return ret;
        }
    }
    api_progress(progress, thread_idx, round, "delete-even end");
    api_progress(progress, thread_idx, round, "lookup-after-delete-even begin");
    for (i = 0; i < entry_num; i++) {
        ret = verify_api_lookup(&entries[i], 1, &main_b, (i % 2) == 0);
        if (ret != 0) {
            return ret;
        }
    }
    api_progress(progress, thread_idx, round, "lookup-after-delete-even end");

    ret = reinsert_even_entries_by_single_loop(entries, entry_num, &main_c,
                                               thread_idx, round, progress);
    if (ret != 0) {
        fprintf(stderr, "synthetic single reinsert failed, ret=%d\n", ret);
        return ret;
    }

    api_progress(progress, thread_idx, round, "lookup-after-single-reinsert begin");
    for (i = 0; i < entry_num; i++) {
        const uvs_eid_t *expected = (i % 2) == 0 ? &main_c : &main_b;

        ret = verify_api_lookup(&entries[i], 1, expected, false);
        if (ret != 0) {
            return ret;
        }
    }
    api_progress(progress, thread_idx, round, "lookup-after-single-reinsert end");

    api_progress(progress, thread_idx, round, "delete-all begin");
    ret = delete_api_entries(entries, entry_num);
    if (ret != 0) {
        return ret;
    }
    api_progress(progress, thread_idx, round, "delete-all end");

    api_progress(progress, thread_idx, round, "lookup-missing begin");
    ret = verify_api_lookup(entries, entry_num, &main_c, true);
    if (ret != 0) {
        return ret;
    }
    api_progress(progress, thread_idx, round, "lookup-missing end");
    return 0;
}

static void *api_thread_main(void *arg)
{
    struct api_thread_ctx *ctx = arg;
    uvs_main_ue_eid_entry_t *entries;
    uint32_t round;

    ctx->status = 0;
    ctx->checked = 0;

    entries = calloc(ctx->entry_num, sizeof(*entries));
    if (entries == NULL) {
        ctx->status = -ENOMEM;
        return NULL;
    }

    for (round = 0; round < ctx->loop; round++) {
        int ret = run_api_round(ctx->thread_idx, round, entries,
                                ctx->entry_num, ctx->progress);

        if (ret != 0) {
            ctx->status = ret;
            free(entries);
            return NULL;
        }
        ctx->checked += ctx->entry_num;
    }

    free(entries);
    return NULL;
}

static int run_api_stress(uint32_t thread_num, uint32_t loop,
                          uint32_t entry_num, bool progress)
{
    pthread_t *threads;
    struct api_thread_ctx *ctxs;
    uint64_t checked = 0;
    uint32_t i;
    int ret = 0;

    if (thread_num == 0) {
        printf("\nskip direct API stress: api-threads=0\n");
        return 0;
    }

    threads = calloc(thread_num, sizeof(*threads));
    ctxs = calloc(thread_num, sizeof(*ctxs));
    if (threads == NULL || ctxs == NULL) {
        free(threads);
        free(ctxs);
        return -ENOMEM;
    }

    printf("\nstart direct API stress: threads=%u loop=%u entries/thread=%u\n",
           thread_num, loop, entry_num);
    fflush(stdout);
    for (i = 0; i < thread_num; i++) {
        int err;

        ctxs[i].thread_idx = i;
        ctxs[i].loop = loop;
        ctxs[i].entry_num = entry_num;
        ctxs[i].progress = progress;
        err = pthread_create(&threads[i], NULL, api_thread_main,
                             &ctxs[i]);
        if (err != 0) {
            ret = -err;
            thread_num = i;
            break;
        }
    }

    for (i = 0; i < thread_num; i++) {
        (void)pthread_join(threads[i], NULL);
        checked += ctxs[i].checked;
        if (ctxs[i].status != 0 && ret == 0) {
            ret = ctxs[i].status;
        }
    }

    free(ctxs);
    free(threads);

    if (ret != 0) {
        fprintf(stderr, "direct API stress failed, checked=%" PRIu64
                ", ret=%d\n", checked, ret);
        return ret;
    }

    printf("direct API stress passed: checked=%" PRIu64 " synthetic entries\n",
           checked);
    return 0;
}

static int check_query_pair_dataset(uint32_t pair_num, uint32_t require_eids,
                                    uint32_t warn_eids)
{
    if (require_eids != 0 && pair_num < require_eids) {
        fprintf(stderr,
                "topo query EIDs %u is smaller than required %u; "
                "run synthetic topo first or lower --require-eids\n",
                pair_num, require_eids);
        return -ERANGE;
    }

    if (warn_eids != 0 && pair_num < warn_eids) {
        fprintf(stderr,
                "warning: only %u topo query EIDs are available, below warn threshold %u\n",
                pair_num, warn_eids);
    }

    return 0;
}

static int prepare_query_pairs(const struct test_options *opts,
                               struct topo_group_record **records_out,
                               uint32_t *record_num_out,
                               struct query_pair **pairs_out,
                               uint32_t *pair_num_out)
{
    struct topo_map_debug *topo = NULL;
    int ret;

    if (opts->expected_topo_file != NULL) {
        ret = load_expected_topo_file(opts->expected_topo_file, &topo);
        if (ret != 0) {
            return ret;
        }
        printf("loaded expected topo file: %s, node_num=%u\n",
               opts->expected_topo_file, topo->node_num);
        printf("skip uvs_set_topo_info refresh; expected pairs use old topo semantics from file\n");
        ret = build_expected_pairs_from_topo(topo, records_out,
                                             record_num_out, pairs_out,
                                             pair_num_out);
        free(topo);
        return ret;
    }

    ret = load_current_topo(&topo);
    if (ret != 0) {
        return ret;
    }

    printf("loaded ubcore topo: node_num=%u\n", topo->node_num);
    if (!opts->skip_set) {
        ret = refresh_table_by_topo(topo);
        if (ret != 0) {
            free(topo);
            return ret;
        }
        printf("refreshed current lookup table through uvs_set_topo_info\n");
    } else {
        printf("skip uvs_set_topo_info; use existing lookup table\n");
    }

    ret = build_expected_pairs_from_topo(topo, records_out, record_num_out,
                                         pairs_out, pair_num_out);
    free(topo);
    return ret;
}

int main(int argc, char **argv)
{
    struct test_options opts;
    struct topo_group_record *records = NULL;
    struct query_pair *pairs = NULL;
    uint32_t record_num = 0;
    uint32_t pair_num = 0;
    int ret;

    ret = parse_args(argc, argv, &opts);
    if (ret != 0) {
        return -ret;
    }

    if (opts.flush_first) {
        ret = uvs_flush_main_ue_eid();
        if (ret != 0) {
            fprintf(stderr, "flush failed, ret=%d\n", ret);
            return -ret;
        }
        printf("flushed main_ue_eid table before test\n");
    }

    ret = prepare_query_pairs(&opts, &records, &record_num, &pairs, &pair_num);
    if (ret != 0) {
        return -ret;
    }

    ret = check_query_pair_dataset(pair_num, opts.require_eids, opts.warn_eids);
    if (ret != 0) {
        free(records);
        free(pairs);
        return -ret;
    }

    print_group_summary(records, record_num);
    ret = validate_multi_primary_group_pairs(records, record_num, pairs,
                                             pair_num,
                                             opts.require_multi_primary_group);
    if (ret != 0) {
        free(records);
        free(pairs);
        return -ret;
    }
    print_pair_preview(pairs, pair_num);
    if (opts.shuffle) {
        shuffle_query_pairs(pairs, pair_num, opts.seed);
        printf("shuffled query pairs with seed=%u\n", opts.seed);
        print_pair_preview(pairs, pair_num);
    }

    ret = validate_all_pairs(pairs, pair_num, "sequential-before");
    if (ret != 0) {
        free(records);
        free(pairs);
        return -ret;
    }
    print_actual_lookup_preview(pairs, pair_num);

    if (!opts.check_only) {
        ret = run_concurrent_lookup(pairs, pair_num, opts.lookup_threads,
                                    opts.lookup_loop);
        if (ret == 0) {
            ret = run_api_stress(opts.api_threads, opts.api_loop,
                                 opts.api_entry_num, opts.api_progress);
        }
        if (ret == 0) {
            ret = validate_all_pairs(pairs, pair_num, "sequential-after");
        }
    }

    free(records);
    free(pairs);
    return ret == 0 ? 0 : -ret;
}
