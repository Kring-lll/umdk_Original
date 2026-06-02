/*
 * SPDX-License-Identifier: MIT
 *
 * Debug test for UVS main UE EID table configuration.
 *
 * Build example after libtpsa is built or installed:
 *
 *   gcc -D_GNU_SOURCE -std=c99 -Wall -Wextra \
 *       -I/home/zdxfgre/jd/umdk/src/urma/lib/uvs/core/include \
 *       -I/home/zdxfgre/jd/umdk/src/urma/common/include \
 *       -I/home/zdxfgre/jd/umdk/src/urma/lib/urma/core/include \
 *       /home/zdxfgre/jd/main_primary_eid_debug_test.c \
 *       -L/usr/lib64 -ltpsa -o /tmp/main_ue_eid_debug_test
 *
 * If libtpsa is not installed, use the directory that
 * contains libtpsa.so in your server build output and add LD_LIBRARY_PATH.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvs_api.h"

#define TEST_MANY_ENTRY_NUM 4113U
#define TEST_ENTRY_BASE_LOW16 0x1000u
#define TEST_FULL_API_NUM 4U
#define TEST_UPDATE_NUM 3U

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

static int fill_entry_from_str(uvs_main_ue_eid_entry_t *entry,
                               const char *eid_str, const char *main_ue)
{
    int ret;

    (void)memset(entry, 0, sizeof(*entry));
    ret = parse_eid(eid_str, &entry->eid);
    if (ret != 0) {
        return ret;
    }

    ret = parse_eid(main_ue, &entry->main_ue_eid);
    if (ret != 0) {
        return ret;
    }
    return 0;
}

static void make_test_eid(uint32_t idx, uvs_eid_t *eid)
{
    (void)memset(eid, 0, sizeof(*eid));
    eid->raw[0] = 0x20;
    eid->raw[1] = 0x01;
    eid->raw[2] = 0x0d;
    eid->raw[3] = 0xb8;
    eid->raw[4] = 0x88;
    eid->raw[5] = 0x66;
    eid->raw[14] = (uint8_t)((TEST_ENTRY_BASE_LOW16 + idx) >> 8);
    eid->raw[15] = (uint8_t)(TEST_ENTRY_BASE_LOW16 + idx);
}

static void print_eid(const char *label, const uvs_eid_t *eid)
{
    char buf[INET6_ADDRSTRLEN] = {0};

    if (inet_ntop(AF_INET6, eid->raw, buf, sizeof(buf)) == NULL) {
        fprintf(stderr, "%s: <inet_ntop failed>\n", label);
        return;
    }

    printf("%s: %s\n", label, buf);
}

static bool eid_equal(const uvs_eid_t *left, const uvs_eid_t *right)
{
    return memcmp(left->raw, right->raw, sizeof(left->raw)) == 0;
}

static int expect_ret(const char *name, int ret, int expected)
{
    if (ret == expected) {
        printf("%s returned %d as expected\n", name, expected);
        return 0;
    }

    fprintf(stderr, "%s returned %d, expected %d\n", name, ret, expected);
    return ret == 0 ? -EINVAL : ret;
}

static int expect_lookup(const uvs_eid_t *eid, const uvs_eid_t *expected)
{
    uvs_eid_t actual = {0};
    int ret;

    ret = uvs_lookup_main_ue_eid(eid, &actual);
    if (ret != 0) {
        fprintf(stderr, "lookup failed, ret=%d\n", ret);
        print_eid("lookup eid", eid);
        return ret;
    }

    if (!eid_equal(&actual, expected)) {
        fprintf(stderr, "lookup result mismatch\n");
        print_eid("lookup eid", eid);
        print_eid("actual main", &actual);
        print_eid("expected main", expected);
        return -EINVAL;
    }

    print_eid("lookup eid", eid);
    print_eid("main ue eid", &actual);
    return 0;
}

static int expect_lookup_missing(const uvs_eid_t *eid)
{
    uvs_eid_t actual = {0};
    int ret;

    ret = uvs_lookup_main_ue_eid(eid, &actual);
    if (ret == -ENOENT) {
        print_eid("lookup missing as expected", eid);
        return 0;
    }

    fprintf(stderr, "lookup missing expectation failed, ret=%d\n", ret);
    print_eid("lookup eid", eid);
    if (ret == 0) {
        print_eid("unexpected main ue eid", &actual);
    }
    return ret == 0 ? -EEXIST : ret;
}

static int set_single_entry(const uvs_eid_t *eid, const uvs_eid_t *main_ue)
{
    uvs_main_ue_eid_entry_t entry = {0};

    entry.eid = *eid;
    entry.main_ue_eid = *main_ue;

    return uvs_insert_main_ue_eid(&entry);
}

static int insert_entries(const uvs_main_ue_eid_entry_t *entries,
                          uint32_t entry_num)
{
    uint32_t i;
    int ret;

    if (entry_num != 0 && entries == NULL) {
        return -EINVAL;
    }

    for (i = 0; i < entry_num; i++) {
        ret = uvs_insert_main_ue_eid(&entries[i]);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static int test_invalid_parameter_interfaces(void)
{
    uvs_eid_t eid = {0};
    uvs_eid_t main_ue = {0};
    int ret;

    printf("\n[0] API invalid parameter checks\n");

    ret = parse_eid("2001:db8:8866::9001", &eid);
    if (ret != 0) {
        return ret;
    }
    ret = expect_ret("insert NULL entry",
                     uvs_insert_main_ue_eid(NULL), -EINVAL);
    if (ret != 0) {
        return ret;
    }

    ret = expect_ret("delete NULL eid", uvs_delete_main_ue_eid(NULL),
                     -EINVAL);
    if (ret != 0) {
        return ret;
    }

    ret = expect_ret("lookup NULL eid",
                     uvs_lookup_main_ue_eid(NULL, &main_ue),
                     -EINVAL);
    if (ret != 0) {
        return ret;
    }

    ret = expect_ret("lookup NULL output",
                     uvs_lookup_main_ue_eid(&eid, NULL), -EINVAL);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

static int test_single_set_and_update(void)
{
    uvs_eid_t eid = {0};
    uvs_eid_t main_first = {0};
    uvs_eid_t main_second = {0};
    int ret;

    ret = parse_eid("2001:db8:8866::10", &eid);
    if (ret != 0) {
        return ret;
    }
    ret = parse_eid("2001:db8:8866::1", &main_first);
    if (ret != 0) {
        return ret;
    }
    ret = parse_eid("2001:db8:8866::2", &main_second);
    if (ret != 0) {
        return ret;
    }

    printf("\n[1] single set and lookup\n");
    ret = set_single_entry(&eid, &main_first);
    if (ret != 0) {
        fprintf(stderr, "single set failed, ret=%d\n", ret);
        return ret;
    }
    ret = expect_lookup(&eid, &main_first);
    if (ret != 0) {
        return ret;
    }

    printf("\n[2] set same EID again to verify incremental overwrite\n");
    ret = set_single_entry(&eid, &main_second);
    if (ret != 0) {
        fprintf(stderr, "single update failed, ret=%d\n", ret);
        return ret;
    }
    ret = expect_lookup(&eid, &main_second);
    if (ret != 0) {
        return ret;
    }

    ret = uvs_delete_main_ue_eid(&eid);
    if (ret != 0) {
        fprintf(stderr, "single cleanup failed, ret=%d\n", ret);
        return ret;
    }

    return expect_lookup_missing(&eid);
}

static int test_incremental_update(void)
{
    static const char *eids[TEST_UPDATE_NUM] = {
        "2001:db8:8866::2101",
        "2001:db8:8866::2102",
        "2001:db8:8866::2103",
    };
    static const char *old_mains[TEST_UPDATE_NUM] = {
        "2001:db8:8866::2201",
        "2001:db8:8866::2201",
        "2001:db8:8866::2201",
    };
    static const char *new_mains[TEST_UPDATE_NUM] = {
        "2001:db8:8866::2301",
        "2001:db8:8866::2302",
        "2001:db8:8866::2303",
    };
    uvs_main_ue_eid_entry_t entries[TEST_UPDATE_NUM] = {0};
    uvs_main_ue_eid_entry_t updates[TEST_UPDATE_NUM] = {0};
    uint32_t i;
    int ret;

    printf("\n[3] incremental overwrite and per-entry delete\n");

    for (i = 0; i < TEST_UPDATE_NUM; i++) {
        ret = fill_entry_from_str(&entries[i], eids[i], old_mains[i]);
        if (ret != 0) {
            return ret;
        }
        ret = fill_entry_from_str(&updates[i], eids[i], new_mains[i]);
        if (ret != 0) {
            return ret;
        }
    }

    ret = insert_entries(entries, TEST_UPDATE_NUM);
    if (ret != 0) {
        fprintf(stderr, "initial insert failed, ret=%d\n", ret);
        return ret;
    }

    for (i = 0; i < TEST_UPDATE_NUM; i++) {
        ret = expect_lookup(&entries[i].eid, &entries[i].main_ue_eid);
        if (ret != 0) {
            return ret;
        }
    }

    ret = insert_entries(updates, TEST_UPDATE_NUM);
    if (ret != 0) {
        fprintf(stderr, "overwrite failed, ret=%d\n", ret);
        return ret;
    }

    for (i = 0; i < TEST_UPDATE_NUM; i++) {
        ret = expect_lookup(&updates[i].eid, &updates[i].main_ue_eid);
        if (ret != 0) {
            return ret;
        }
    }

    ret = uvs_delete_main_ue_eid(&updates[1].eid);
    if (ret != 0) {
        fprintf(stderr, "delete middle updated entry failed, ret=%d\n", ret);
        return ret;
    }

    ret = expect_lookup(&updates[0].eid, &updates[0].main_ue_eid);
    if (ret != 0) {
        return ret;
    }
    ret = expect_lookup_missing(&updates[1].eid);
    if (ret != 0) {
        return ret;
    }
    ret = expect_lookup(&updates[2].eid, &updates[2].main_ue_eid);
    if (ret != 0) {
        return ret;
    }

    for (i = 0; i < TEST_UPDATE_NUM; i++) {
        ret = uvs_delete_main_ue_eid(&updates[i].eid);
        if (ret != 0 && ret != -ENOENT) {
            fprintf(stderr, "cleanup updated entry %u failed, ret=%d\n", i, ret);
            return ret;
        }
    }

    for (i = 0; i < TEST_UPDATE_NUM; i++) {
        ret = expect_lookup_missing(&updates[i].eid);
        if (ret != 0) {
            return ret;
        }
    }

    return 0;
}

static void fill_many_entries(uvs_main_ue_eid_entry_t *entries,
                              uint32_t entry_num, const uvs_eid_t *main_ue)
{
    uint32_t i;

    for (i = 0; i < entry_num; i++) {
        make_test_eid(i, &entries[i].eid);
        entries[i].main_ue_eid = *main_ue;
    }
}

static int cleanup_many_entries(uint32_t entry_num)
{
    uvs_eid_t eid = {0};
    uint32_t i;
    int ret;

    for (i = 0; i < entry_num; i++) {
        make_test_eid(i, &eid);
        ret = uvs_delete_main_ue_eid(&eid);
        if (ret != 0 && ret != -ENOENT) {
            fprintf(stderr, "cleanup entry %u failed, ret=%d\n", i, ret);
            return ret;
        }
    }

    return 0;
}

static int test_delete_flush_interfaces(void)
{
    static const char *eids[TEST_FULL_API_NUM] = {
        "2001:db8:8866::3001",
        "2001:db8:8866::3002",
        "2001:db8:8866::3003",
        "2001:db8:8866::3004",
    };
    static const char *mains[TEST_FULL_API_NUM] = {
        "2001:db8:8866::3101",
        "2001:db8:8866::3101",
        "2001:db8:8866::3201",
        "2001:db8:8866::3201",
    };
    uvs_main_ue_eid_entry_t entries[TEST_FULL_API_NUM] = {0};
    uvs_eid_t stale_eid = {0};
    uvs_eid_t stale_main = {0};
    uint32_t i;
    int ret;

    printf("\n[4] full API: insert, delete missing, flush\n");
    printf("this subtest flushes the whole main UE EID table\n");

    ret = uvs_flush_main_ue_eid();
    if (ret != 0) {
        fprintf(stderr, "initial full-api flush failed, ret=%d\n", ret);
        return ret;
    }

    ret = expect_ret("idempotent flush on empty table",
                     uvs_flush_main_ue_eid(), 0);
    if (ret != 0) {
        return ret;
    }

    ret = parse_eid("2001:db8:8866::2fff", &stale_eid);
    if (ret != 0) {
        return ret;
    }
    ret = parse_eid("2001:db8:8866::2ffe", &stale_main);
    if (ret != 0) {
        return ret;
    }
    ret = set_single_entry(&stale_eid, &stale_main);
    if (ret != 0) {
        fprintf(stderr, "set stale entry failed, ret=%d\n", ret);
        return ret;
    }
    ret = expect_lookup(&stale_eid, &stale_main);
    if (ret != 0) {
        return ret;
    }

    for (i = 0; i < TEST_FULL_API_NUM; i++) {
        ret = fill_entry_from_str(&entries[i], eids[i], mains[i]);
        if (ret != 0) {
            return ret;
        }
    }

    ret = insert_entries(entries, TEST_FULL_API_NUM);
    if (ret != 0) {
        fprintf(stderr, "set entries failed, ret=%d\n", ret);
        return ret;
    }

    ret = expect_lookup(&stale_eid, &stale_main);
    if (ret != 0) {
        return ret;
    }

    for (i = 0; i < TEST_FULL_API_NUM; i++) {
        ret = expect_lookup(&entries[i].eid, &entries[i].main_ue_eid);
        if (ret != 0) {
            return ret;
        }
    }

    ret = uvs_delete_main_ue_eid(&entries[0].eid);
    if (ret != 0) {
        fprintf(stderr, "delete entry failed, ret=%d\n", ret);
        return ret;
    }
    ret = expect_lookup_missing(&entries[0].eid);
    if (ret != 0) {
        return ret;
    }
    ret = uvs_delete_main_ue_eid(&entries[0].eid);
    if (ret != -ENOENT) {
        fprintf(stderr, "delete missing entry returned %d, expected %d\n",
                ret, -ENOENT);
        return ret == 0 ? -EINVAL : ret;
    }
    printf("delete missing entry returned -ENOENT as expected\n");

    ret = uvs_flush_main_ue_eid();
    if (ret != 0) {
        fprintf(stderr, "flush failed, ret=%d\n", ret);
        return ret;
    }
    ret = expect_lookup_missing(&stale_eid);
    if (ret != 0) {
        return ret;
    }
    for (i = 0; i < TEST_FULL_API_NUM; i++) {
        ret = expect_lookup_missing(&entries[i].eid);
        if (ret != 0) {
            return ret;
        }
    }

    printf("full API insert/delete/flush subtest passed\n");
    return 0;
}

static int test_many_single_inserts(bool keep_entries)
{
    uvs_main_ue_eid_entry_t *entries = NULL;
    uvs_eid_t main_ue = {0};
    uvs_eid_t first_eid = {0};
    uvs_eid_t mid_eid = {0};
    uvs_eid_t last_eid = {0};
    int ret;

    printf("\n[5] set %u entries by single insert API\n", TEST_MANY_ENTRY_NUM);

    entries = calloc(TEST_MANY_ENTRY_NUM, sizeof(*entries));
    if (entries == NULL) {
        return -ENOMEM;
    }

    ret = parse_eid("2001:db8:8866::ffff", &main_ue);
    if (ret != 0) {
        free(entries);
        return ret;
    }

    fill_many_entries(entries, TEST_MANY_ENTRY_NUM, &main_ue);

    ret = insert_entries(entries, TEST_MANY_ENTRY_NUM);
    if (ret != 0) {
        fprintf(stderr, "set entries failed, ret=%d\n", ret);
        free(entries);
        return ret;
    }

    make_test_eid(0, &first_eid);
    make_test_eid(TEST_MANY_ENTRY_NUM / 2, &mid_eid);
    make_test_eid(TEST_MANY_ENTRY_NUM - 1, &last_eid);
    ret = expect_lookup(&first_eid, &main_ue);
    if (ret != 0) {
        free(entries);
        return ret;
    }
    ret = expect_lookup(&mid_eid, &main_ue);
    if (ret != 0) {
        free(entries);
        return ret;
    }
    ret = expect_lookup(&last_eid, &main_ue);
    if (ret != 0) {
        free(entries);
        return ret;
    }

    free(entries);
    if (keep_entries) {
        printf("keep entries enabled, skip cleanup\n");
        return 0;
    }

    printf("cleanup %u test entries\n", TEST_MANY_ENTRY_NUM);
    ret = cleanup_many_entries(TEST_MANY_ENTRY_NUM);
    if (ret != 0) {
        return ret;
    }

    ret = expect_lookup_missing(&first_eid);
    if (ret != 0) {
        return ret;
    }
    ret = expect_lookup_missing(&mid_eid);
    if (ret != 0) {
        return ret;
    }
    return expect_lookup_missing(&last_eid);
}

static void usage(const char *prog)
{
    printf("Usage: %s [--flush-first] [--keep] [--full-api]\n", prog);
    printf("  --flush-first  flush the whole main UE EID table before test\n");
    printf("  --keep         keep inserted test entries for manual lookup/debug\n");
    printf("  --full-api     also test insert/delete/flush APIs; this clears the whole table\n");
}

int main(int argc, char **argv)
{
    bool flush_first = false;
    bool keep_entries = false;
    bool full_api = false;
    int ret;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flush-first") == 0) {
            flush_first = true;
        } else if (strcmp(argv[i], "--keep") == 0) {
            keep_entries = true;
        } else if (strcmp(argv[i], "--full-api") == 0) {
            full_api = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return EINVAL;
        }
    }

    if (flush_first) {
        printf("flush table before test\n");
        ret = uvs_flush_main_ue_eid();
        if (ret != 0) {
            fprintf(stderr, "flush failed, ret=%d\n", ret);
            return -ret;
        }
    }

    ret = test_invalid_parameter_interfaces();
    if (ret != 0) {
        return -ret;
    }

    ret = test_single_set_and_update();
    if (ret != 0) {
        return -ret;
    }

    ret = test_incremental_update();
    if (ret != 0) {
        return -ret;
    }

    if (full_api) {
        ret = test_delete_flush_interfaces();
        if (ret != 0) {
            return -ret;
        }
    }

    ret = test_many_single_inserts(keep_entries);
    if (ret != 0) {
        return -ret;
    }

    printf("\nmain UE EID debug test passed\n");
    return 0;
}
