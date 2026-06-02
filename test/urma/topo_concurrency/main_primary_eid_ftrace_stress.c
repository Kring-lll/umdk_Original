/*
 * SPDX-License-Identifier: MIT
 *
 * Stress caller for tracing the main UE EID lookup path with ftrace.
 *
 * This program reloads the current topo through uvs_set_topo_info() in a loop.
 * The ubcore/ubmad set-topo path calls the main UE EID lookup helper, so
 * function_graph ftrace can be filtered to that kernel function.
 */

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "uvs_api.h"

#define DEFAULT_LOOP 1000U
#define DEFAULT_PRINT_EVERY 100U

enum stress_mode {
    STRESS_MODE_SET_TOPO,
    STRESS_MODE_LOOKUP,
};

struct topo_map_debug {
    struct urma_topo_node topo_infos[MAX_NODE_NUM];
    uint32_t node_num;
};

struct test_options {
    uint64_t loop;
    uint32_t warmup;
    uint32_t print_every;
    uint32_t sleep_us;
    enum stress_mode mode;
};

static volatile sig_atomic_t g_stop;

static void handle_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint64_t get_time_us(void)
{
    struct timeval tv;

    (void)gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static bool raw_eid_valid(const char *raw)
{
    const char empty[EID_LEN] = {0};

    return raw != NULL && memcmp(raw, empty, EID_LEN) != 0;
}

static void copy_raw_eid_to_uvs_eid(const char *raw, uvs_eid_t *eid)
{
    (void)memset(eid, 0, sizeof(*eid));
    (void)memcpy(eid->raw, raw, EID_LEN);
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

static int parse_u64_arg(const char *name, const char *value, uint64_t *out)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        return -EINVAL;
    }

    *out = (uint64_t)parsed;
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --mode set-topo|lookup\n");
    printf("                    set-topo reloads topo, lookup repeatedly calls uvs_lookup_main_ue_eid\n");
    printf("                    default set-topo\n");
    printf("  --loop N          set_topo loop count, default %u; 0 means until signal\n",
           DEFAULT_LOOP);
    printf("  --warmup N        warmup set_topo calls before measurement, default 0\n");
    printf("  --print-every N   print progress every N calls, default %u; 0 disables it\n",
           DEFAULT_PRINT_EVERY);
    printf("  --sleep-us N      sleep between set_topo calls, default 0\n");
    printf("  -h, --help        show this help\n");
}

static int parse_args(int argc, char **argv, struct test_options *opts)
{
    int i;

    opts->loop = DEFAULT_LOOP;
    opts->warmup = 0;
    opts->print_every = DEFAULT_PRINT_EVERY;
    opts->sleep_us = 0;
    opts->mode = STRESS_MODE_SET_TOPO;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];

            if (strcmp(mode, "set-topo") == 0) {
                opts->mode = STRESS_MODE_SET_TOPO;
            } else if (strcmp(mode, "lookup") == 0) {
                opts->mode = STRESS_MODE_LOOKUP;
            } else {
                fprintf(stderr, "invalid --mode: %s\n", mode);
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--loop") == 0 && i + 1 < argc) {
            if (parse_u64_arg("--loop", argv[++i], &opts->loop) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--warmup", argv[++i], &opts->warmup) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--print-every") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--print-every", argv[++i],
                              &opts->print_every) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--sleep-us") == 0 && i + 1 < argc) {
            if (parse_u32_arg("--sleep-us", argv[++i], &opts->sleep_us) != 0) {
                return -EINVAL;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return -EINVAL;
        }
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

static int refresh_topo(const struct topo_map_debug *topo)
{
    return uvs_set_topo_info((void *)topo->topo_infos,
                             sizeof(struct urma_topo_node),
                             topo->node_num);
}

static int find_lookup_eid(const struct topo_map_debug *topo, uvs_eid_t *eid)
{
    uint32_t node_idx;
    uint32_t dev_idx;
    uint32_t iodie_idx;
    uint32_t port_idx;

    for (node_idx = 0; node_idx < topo->node_num; node_idx++) {
        const struct urma_topo_node *node = &topo->topo_infos[node_idx];

        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            const struct urma_topo_agg_dev *dev = &node->agg_devs[dev_idx];

            for (iodie_idx = 0; iodie_idx < IODIE_NUM; iodie_idx++) {
                const struct urma_topo_ue *ue = &dev->ues[iodie_idx];

                if (raw_eid_valid(ue->primary_eid)) {
                    copy_raw_eid_to_uvs_eid(ue->primary_eid, eid);
                    return 0;
                }

                for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
                    if (raw_eid_valid(ue->port_eid[port_idx])) {
                        copy_raw_eid_to_uvs_eid(ue->port_eid[port_idx], eid);
                        return 0;
                    }
                }
            }
        }
    }

    return -ENOENT;
}

static int do_one_call(const struct topo_map_debug *topo,
                       const struct test_options *opts, const uvs_eid_t *eid)
{
    uvs_eid_t main_ue_eid = {0};

    if (opts->mode == STRESS_MODE_LOOKUP)
        return uvs_lookup_main_ue_eid(eid, &main_ue_eid);

    return refresh_topo(topo);
}

static int run_warmup(const struct topo_map_debug *topo,
                      const struct test_options *opts, const uvs_eid_t *eid)
{
    uint32_t i;

    for (i = 0; i < opts->warmup && !g_stop; i++) {
        int ret = do_one_call(topo, opts, eid);

        if (ret != 0) {
            fprintf(stderr, "warmup call failed, idx=%u ret=%d\n",
                    i, ret);
            return ret;
        }
    }

    return 0;
}

static int run_stress(const struct topo_map_debug *topo,
                      const struct test_options *opts, const uvs_eid_t *eid)
{
    uint64_t begin_us;
    uint64_t done = 0;
    int ret = 0;

    begin_us = get_time_us();
    while (!g_stop && (opts->loop == 0 || done < opts->loop)) {
        ret = do_one_call(topo, opts, eid);
        if (ret != 0) {
            fprintf(stderr, "stress call failed, done=%" PRIu64 " ret=%d\n",
                    done, ret);
            return ret;
        }

        done++;
        if (opts->print_every != 0 && done % opts->print_every == 0) {
            uint64_t elapsed_us = get_time_us() - begin_us;
            double avg_us = elapsed_us == 0 ? 0.0 : (double)elapsed_us / done;

            printf("progress: calls=%" PRIu64 " elapsed_us=%" PRIu64
                   " avg_user_set_topo_us=%.3f\n", done, elapsed_us, avg_us);
            fflush(stdout);
        }
        if (opts->sleep_us != 0) {
            (void)usleep(opts->sleep_us);
        }
    }

    printf("finished: calls=%" PRIu64 " elapsed_us=%" PRIu64 "\n",
           done, get_time_us() - begin_us);
    return 0;
}

int main(int argc, char **argv)
{
    struct test_options opts;
    struct topo_map_debug *topo = NULL;
    uvs_eid_t lookup_eid = {0};
    int ret;

    (void)signal(SIGINT, handle_signal);
    (void)signal(SIGTERM, handle_signal);

    ret = parse_args(argc, argv, &opts);
    if (ret != 0) {
        return -ret;
    }

    ret = load_current_topo(&topo);
    if (ret != 0) {
        return -ret;
    }
    printf("loaded topo: node_num=%u\n", topo->node_num);

    if (opts.mode == STRESS_MODE_LOOKUP) {
        ret = refresh_topo(topo);
        if (ret != 0) {
            fprintf(stderr, "prepare lookup table failed, ret=%d\n", ret);
            free(topo);
            return -ret;
        }
        ret = find_lookup_eid(topo, &lookup_eid);
        if (ret != 0) {
            fprintf(stderr, "failed to find lookup eid, ret=%d\n", ret);
            free(topo);
            return -ret;
        }
        printf("stress mode: lookup\n");
    } else {
        printf("stress mode: set-topo\n");
    }

    ret = run_warmup(topo, &opts, &lookup_eid);
    if (ret == 0) {
        ret = run_stress(topo, &opts, &lookup_eid);
    }

    free(topo);
    return ret == 0 ? 0 : -ret;
}
