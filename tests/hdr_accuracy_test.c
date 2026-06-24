/*
 * SPDX-License-Identifier: MIT
 * Standalone HDR accuracy self-test.
 * Generates latency-distributed samples, compares HDR quantiles against an
 * exact brute-force (sort + nearest-rank) reference, and prints the error.
 *
 * Build: see run_hdr_test.sh
 * Usage: ./hdr_accuracy_test [seed] [n_samples]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <inttypes.h>

#include "hdr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_SEED    (0x9e3779b97f4a7c15ULL)
#define DEFAULT_N       (1000000ULL)
/* HDR sig=2 theoretical worst-case relative error = 1/128 ~= 0.78125% */
#define ERR_THRESHOLD   (1.0)

/* ---- deterministic xorshift64 RNG ---- */
static uint64_t g_rng_state;
static void rng_seed(uint64_t s) { g_rng_state = s ? s : 1; }
static uint64_t xorshift64(void)
{
    uint64_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    g_rng_state = x;
    return x;
}
static double rand_unit(void)
{
    /* uniform [0,1) with 53-bit mantissa */
    return (double)(xorshift64() >> 11) / (double)(1ULL << 53);
}

/* Box-Muller normal(0,1) */
static double rand_normal(void)
{
    double u1 = rand_unit();
    double u2 = rand_unit();
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/*
 * Generate one latency sample (in CPU cycles) following a log-normal
 * distribution (the canonical model for request latency) with a long tail,
 * plus rare spikes to stress p9999. Values are clamped below
 * HDR_HIGHEST_TRACKABLE so the precision comparison is not distorted by
 * HDR's max-value clamping.
 */
static uint64_t gen_latency(void)
{
    double mu = log(300.0);     /* median ~ 300 cycles */
    double sigma = 1.1;         /* spread */
    double v = exp(mu + sigma * rand_normal());
    if (rand_unit() < 0.0002) { /* 0.02% extreme spikes */
        v *= 40.0;
    }
    if (v < 1.0) {
        v = 1.0;
    }
    uint64_t iv = (uint64_t)(v + 0.5);
    if (iv >= HDR_HIGHEST_TRACKABLE) {
        iv = HDR_HIGHEST_TRACKABLE - 1;
    }
    return iv;
}

/* exact nearest-rank quantile on an ascending-sorted array */
static uint64_t exact_quantile(const uint64_t *sorted, uint64_t n, double pct)
{
    double target = (pct / 100.0) * (double)n;
    uint64_t rank = (uint64_t)target;
    if ((double)rank < target) {
        rank++;
    }
    if (rank < 1) {
        rank = 1;
    }
    if (rank > n) {
        rank = n;
    }
    return sorted[rank - 1];
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = DEFAULT_SEED;
    uint64_t n = DEFAULT_N;
    if (argc >= 2) {
        seed = (uint64_t)strtoull(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        n = (uint64_t)strtoull(argv[2], NULL, 0);
        if (n < 100) {
            n = 100;
        }
    }

    rng_seed(seed);

    uint64_t *data = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (data == NULL) {
        fprintf(stderr, "malloc failed\n");
        return 2;
    }

    /* 1) generate latency-distributed data */
    uint64_t data_min = UINT64_MAX;
    uint64_t data_max = 0;
    double data_sum = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v = gen_latency();
        data[i] = v;
        if (v < data_min) data_min = v;
        if (v > data_max) data_max = v;
        data_sum += (double)v;
    }

    /* 2) record every sample into HDR */
    hdr_histogram_t *h = hdr_create();
    if (h == NULL) {
        fprintf(stderr, "hdr_create failed\n");
        free(data);
        return 2;
    }
    for (uint64_t i = 0; i < n; i++) {
        hdr_record(h, data[i]);
    }

    printf("=== HDR accuracy self-test ===\n");
    printf("seed=%" PRIu64 "  n_samples=%" PRIu64 "\n", seed, n);
    printf("[data] min=%" PRIu64 " max=%" PRIu64 " mean=%.2f\n",
           data_min, data_max, data_sum / (double)n);

    /* basic functionality sanity: count/min/max/sum must match */
    int sanity_fail = 0;
    if (h->total_count != n) {
        printf("[sanity] FAIL total_count hdr=%" PRIu64 " expected=%" PRIu64 "\n",
               h->total_count, n);
        sanity_fail = 1;
    }
    if (h->min_value != data_min) {
        printf("[sanity] FAIL min hdr=%" PRIu64 " expected=%" PRIu64 "\n",
               h->min_value, data_min);
        sanity_fail = 1;
    }
    if (h->max_value != data_max) {
        printf("[sanity] FAIL max hdr=%" PRIu64 " expected=%" PRIu64 "\n",
               h->max_value, data_max);
        sanity_fail = 1;
    }
    if (sanity_fail == 0) {
        printf("[sanity] OK (total_count/min/max match)\n");
    }

    /* memory footprint */
    size_t buckets = hdr_counts_size();
    size_t bytes = buckets * sizeof(uint64_t);
    printf("[mem] buckets=%zu counts_array=%zu bytes (%.1f KB)\n",
           buckets, bytes, (double)bytes / 1024.0);

    /* 3) exact reference: sort then nearest-rank */
    qsort(data, n, sizeof(uint64_t), cmp_u64);

    /* 4) compare HDR vs exact for p50/p90/p99/p9999 */
    double pcts[] = {50.0, 90.0, 99.0, 99.99};
    const char *names[] = {"p50", "p90", "p99", "p9999"};

    printf("[precision]\n");
    printf("%-8s %-16s %-16s %-12s %-10s\n",
           "quantile", "exact", "hdr", "rel_err(%)", "verdict");
    int prec_fail = 0;
    double worst = 0.0;
    for (int k = 0; k < 4; k++) {
        uint64_t exact = exact_quantile(data, n, pcts[k]);
        uint64_t got = hdr_value_at_quantile(h, pcts[k]);
        double rel = 0.0;
        if (exact != 0) {
            double diff = (double)((int64_t)got - (int64_t)exact);
            rel = 100.0 * diff / (double)exact;
            if (rel < 0.0) rel = -rel;
        }
        if (rel > worst) worst = rel;
        const char *verdict = (rel <= ERR_THRESHOLD) ? "OK" : "FAIL";
        if (rel > ERR_THRESHOLD) prec_fail = 1;
        printf("%-8s %-16" PRIu64 " %-16" PRIu64 " %-12.4f %-10s\n",
               names[k], exact, got, rel, verdict);
    }
    printf("[precision] worst rel_err=%.4f%% (threshold=%.2f%%)\n", worst, ERR_THRESHOLD);

    hdr_destroy(h);
    free(data);

    int fail = sanity_fail || prec_fail;
    printf("\n[verdict] %s\n", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
