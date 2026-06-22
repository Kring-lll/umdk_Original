/**
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* t-digest algorithm C implementation
*/
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "tdigest.h"

static inline double tdigest_lerp(double a, double b, double t)
{
    return a + t * (b - a);
}

static void list_init(tdigest_centroid_list_t *list, tdigest_centroid_t *arr, size_t size)
{
    list->centroids = arr;
    list->capacity = size;
    list->size = 0;
    list->total_weight = 0;
}

static void list_reset(tdigest_centroid_list_t *list)
{
    list->size = 0;
    list->total_weight = 0;
}

static tdigest_insert_result_e list_insert(tdigest_centroid_list_t *list, double mean, uint32_t weight)
{
    // reject out-of-range values
    if (mean < 0 || mean > (double)UINT32_MAX) {
        return TDIGEST_NO_NEED_COMPRESS;
    }
    if (list->size >= list->capacity) {
        return TDIGEST_OVERFLOW;
    }
    list->centroids[list->size].mean = mean;
    list->centroids[list->size].weight = weight;
    list->size++;
    list->total_weight += weight;
    return TDIGEST_NO_NEED_COMPRESS;
}

static double compute_normalizer(double compression, double n)
{
    if (n <= 0.0 || compression <= 0.0) {
        return 1.0;
    }
    double denom = TDIGEST_NORMALIZER_K4 * log(n / compression) + TDIGEST_NORMALIZER_K21;
    if (denom <= 0.0) {
        return 1.0;
    }
    return compression / denom;
}

static double quantile_to_scale(double q, double normalizer)
{
    if (q < TDIGEST_QMIN) {
        return TDIGEST_SCALE_FACTOR * quantile_to_scale(TDIGEST_QMIN, normalizer);
    } else if (q > TDIGEST_QMAX) {
        return TDIGEST_SCALE_FACTOR * quantile_to_scale(TDIGEST_QMAX, normalizer);
    }
    if (q <= TDIGEST_QMID) {
        return log(TDIGEST_SCALE_FACTOR * q) * normalizer;
    } else {
        return -log(TDIGEST_SCALE_FACTOR * (1.0 - q)) * normalizer;
    }
}

static double scale_to_quantile(double k, double normalizer)
{
    if (normalizer <= 0.0) {
        return 0.0;
    }
    if (k <= 0) {
        return exp(k / normalizer) / TDIGEST_SCALE_FACTOR;
    } else {
        return 1.0 - exp(-k / normalizer) / TDIGEST_SCALE_FACTOR;
    }
}

static int cmp_centroid_asc(const void *a, const void *b)
{
    const tdigest_centroid_t *ca = (const tdigest_centroid_t *)a;
    const tdigest_centroid_t *cb = (const tdigest_centroid_t *)b;
    if (ca->mean < cb->mean) {
        return -1;
    }
    if (ca->mean > cb->mean) {
        return 1;
    }
    return 0;
}

static int cmp_centroid_desc(const void *a, const void *b)
{
    const tdigest_centroid_t *ca = (const tdigest_centroid_t *)a;
    const tdigest_centroid_t *cb = (const tdigest_centroid_t *)b;
    if (ca->mean > cb->mean) {
        return -1;
    }
    if (ca->mean < cb->mean) {
        return 1;
    }
    return 0;
}

typedef struct compression_state {
    double k1;
    double next_q_limit_weight;
    double weight_so_far;
    double weight_to_add;
    double mean_to_add;
    uint64_t new_total_weight;
    double normalizer;
} compression_state_t;

static void compression_state_init(compression_state_t *state, uint64_t total_weight, double normalizer)
{
    state->new_total_weight = total_weight;
    state->normalizer = normalizer;
    state->k1 = quantile_to_scale(0.0, normalizer);
    state->next_q_limit_weight = (double)total_weight * scale_to_quantile(state->k1 + 1.0, normalizer);
    state->weight_so_far = 0;
    state->weight_to_add = 0;
    state->mean_to_add = 0;
}

static void update_quantile_limit(compression_state_t *state)
{
    double camel_back = (state->new_total_weight > 0) ?
                        state->weight_so_far / (double)state->new_total_weight : 0.0;
    state->k1 = quantile_to_scale(camel_back, state->normalizer);
    state->next_q_limit_weight = (double)state->new_total_weight *
                                 scale_to_quantile(state->k1 + 1.0, state->normalizer);
}

static void merge_into_last_centroid(tdigest_centroid_list_t *list, double mean, uint32_t weight)
{
    if (list->size == 0) {
        return;
    }
    tdigest_centroid_t *last = &list->centroids[list->size - 1];
    double new_weight = (double)last->weight + (double)weight;
    if (new_weight > 0.0) {
        last->mean = (last->mean * last->weight + mean * weight) / new_weight;
    }
    last->weight = (uint32_t)new_weight;
    // The overflowed centroid was never inserted (list_insert returned
    // OVERFLOW), so its weight was not counted in total_weight yet.
    list->total_weight += weight;
}

static void process_centroid(const tdigest_centroid_t *current, compression_state_t *state,
    tdigest_centroid_list_t *inactive)
{
    if ((state->weight_so_far + state->weight_to_add + current->weight) <= state->next_q_limit_weight) {
        state->weight_to_add += current->weight;
        state->mean_to_add += (current->mean - state->mean_to_add) *
                              ((double)current->weight / state->weight_to_add);
    } else {
        state->weight_so_far += state->weight_to_add;
        update_quantile_limit(state);
        if (list_insert(inactive, state->mean_to_add, (uint32_t)state->weight_to_add) == TDIGEST_OVERFLOW) {
            merge_into_last_centroid(inactive, state->mean_to_add, (uint32_t)state->weight_to_add);
        }
        state->mean_to_add = current->mean;
        state->weight_to_add = current->weight;
    }
}

static void compress_data(const tdigest_centroid_list_t *input, tdigest_centroid_list_t *inactive,
                          tdigest_centroid_list_t *buffer_list, const tdigest_centroid_list_t *active)
{
    uint64_t new_total_weight = buffer_list->total_weight + active->total_weight;
    double normalizer = compute_normalizer((double)inactive->capacity, (double)new_total_weight);
    compression_state_t state;
    compression_state_init(&state, new_total_weight, normalizer);

    state.weight_to_add = input->centroids[0].weight;
    state.mean_to_add = input->centroids[0].mean;

    for (size_t i = 1; i < input->size; i++) {
        process_centroid(&input->centroids[i], &state, inactive);
    }
    if ((uint64_t)state.weight_to_add != 0) {
        if (list_insert(inactive, state.mean_to_add, (uint32_t)state.weight_to_add) == TDIGEST_OVERFLOW) {
            merge_into_last_centroid(inactive, state.mean_to_add, (uint32_t)state.weight_to_add);
        }
    }
}

static void update_min_max_forward(const tdigest_centroid_list_t *input, uint64_t *min_val, uint64_t *max_val)
{
    if (input->centroids[0].weight == TDIGEST_WEIGHT_SINGLE) {
        uint64_t cand = (uint64_t)input->centroids[0].mean;
        if (cand < *min_val) {
            *min_val = cand;
        }
    }
    if (input->centroids[input->size - 1].weight == TDIGEST_WEIGHT_SINGLE) {
        uint64_t cand = (uint64_t)input->centroids[input->size - 1].mean;
        if (cand > *max_val) {
            *max_val = cand;
        }
    }
}

static void update_min_max_backward(const tdigest_centroid_list_t *input, uint64_t *min_val, uint64_t *max_val)
{
    if (input->centroids[input->size - 1].weight == TDIGEST_WEIGHT_SINGLE) {
        uint64_t cand = (uint64_t)input->centroids[input->size - 1].mean;
        if (cand < *min_val) {
            *min_val = cand;
        }
    }
    if (input->centroids[0].weight == TDIGEST_WEIGHT_SINGLE) {
        uint64_t cand = (uint64_t)input->centroids[0].mean;
        if (cand > *max_val) {
            *max_val = cand;
        }
    }
}

static void tdigest_merge_internal(tdigest_t *td)
{
    tdigest_centroid_list_t *input = &td->buffer;
    if (input->size == 0) {
        return;
    }

    if (td->forward) {
        qsort(input->centroids, input->size, sizeof(tdigest_centroid_t), cmp_centroid_asc);
    } else {
        qsort(input->centroids, input->size, sizeof(tdigest_centroid_t), cmp_centroid_desc);
    }

    if (td->forward) {
        update_min_max_forward(input, &td->min_value, &td->max_value);
    } else {
        update_min_max_backward(input, &td->min_value, &td->max_value);
    }

    tdigest_centroid_list_t *inactive = (td->active == &td->one) ? &td->two : &td->one;
    compress_data(input, inactive, &td->buffer, td->active);

    if (!td->forward) {
        qsort(inactive->centroids, inactive->size, sizeof(tdigest_centroid_t), cmp_centroid_asc);
    }
    td->forward = !td->forward;
    list_reset(&td->buffer);

    memcpy(input->centroids, inactive->centroids, inactive->size * sizeof(tdigest_centroid_t));
    input->size = inactive->size;
    // total_weight not copied: buffer tracks only new samples since last merge

    tdigest_centroid_list_t *old_active = td->active;
    td->active = inactive;
    list_reset(old_active);
}

tdigest_t *tdigest_create(int compression)
{
    int comp = compression;
    if (comp < 1) {
        comp = 1;
    }
    if (comp > TDIGEST_MAX_COMPRESSION) {
        return NULL;
    }

    tdigest_t *td = (tdigest_t *)calloc(1, sizeof(tdigest_t));
    if (td == NULL) {
        return NULL;
    }

    size_t n1 = (size_t)comp;
    size_t n2 = (size_t)(TDIGEST_BUFFER_RATIO * comp);
    td->one_arr    = (tdigest_centroid_t *)calloc(n1, sizeof(tdigest_centroid_t));
    td->two_arr    = (tdigest_centroid_t *)calloc(n1, sizeof(tdigest_centroid_t));
    td->buffer_arr = (tdigest_centroid_t *)calloc(n2, sizeof(tdigest_centroid_t));
    if (td->one_arr == NULL || td->two_arr == NULL || td->buffer_arr == NULL) {
        free(td->one_arr);
        free(td->two_arr);
        free(td->buffer_arr);
        free(td);
        return NULL;
    }

    list_init(&td->one,    td->one_arr,    n1);
    list_init(&td->two,    td->two_arr,    n1);
    list_init(&td->buffer, td->buffer_arr, n2);
    td->compression = comp;
    td->active = &td->one;
    td->min_value = UINT64_MAX;
    td->max_value = 0;
    td->forward = true;
    return td;
}

void tdigest_destroy(tdigest_t *td)
{
    if (td == NULL) {
        return;
    }
    free(td->one_arr);
    free(td->two_arr);
    free(td->buffer_arr);
    free(td);
}

void tdigest_reset(tdigest_t *td)
{
    if (td == NULL) {
        return;
    }
    list_reset(&td->one);
    list_reset(&td->two);
    list_reset(&td->buffer);
    td->active = &td->one;
    td->min_value = UINT64_MAX;
    td->max_value = 0;
    td->forward = true;
}

void tdigest_insert(tdigest_t *td, double value, uint32_t weight)
{
    if (td == NULL) {
        return;
    }
    tdigest_insert_result_e res = list_insert(&td->buffer, value, weight);
    if (res == TDIGEST_OVERFLOW) {
        tdigest_merge_internal(td);
        res = list_insert(&td->buffer, value, weight);
        if (res == TDIGEST_OVERFLOW) {
            tdigest_merge_internal(td);
            list_insert(&td->buffer, value, weight);
        }
    }
}

static void tdigest_flush(tdigest_t *td)
{
    if (td->buffer.size > 0) {
        tdigest_merge_internal(td);
    }
}

double tdigest_quantile(tdigest_t *td, double p)
{
    if (td == NULL || td->active == NULL) {
        return 0.0;
    }
    if (p < 0.0 || p > TDIGEST_PCT_MAX) {
        return 0.0;
    }

    tdigest_flush(td);
    const tdigest_centroid_list_t *active = td->active;
    if (active->size == 0) {
        return 0.0;
    }
    if (active->size == 1) {
        return active->centroids[0].mean;
    }

    uint64_t total = active->total_weight;
    uint64_t index = (uint64_t)((p / TDIGEST_PCT_MAX) * (double)total);

    if (index < 1) {
        return (double)td->min_value;
    }
    if (index > total - 1) {
        return (double)td->max_value;
    }

    const tdigest_centroid_t *first = &active->centroids[0];
    if (first->weight > 1 && index < (uint64_t)(first->weight / TDIGEST_HALF_WEIGHT_DIV)) {
        double t = (double)(index - 1) / (double)(first->weight / TDIGEST_HALF_WEIGHT_DIV - 1);
        return tdigest_lerp((double)td->min_value, first->mean, t);
    }

    const tdigest_centroid_t *last = &active->centroids[active->size - 1];
    if (last->weight > 1 && total - index <= (uint64_t)(last->weight / TDIGEST_HALF_WEIGHT_DIV)) {
        if ((uint64_t)(last->weight / TDIGEST_HALF_WEIGHT_DIV) <= 1) {
            return last->mean;
        }
        double t = (double)(total - index - 1) / (double)(last->weight / TDIGEST_HALF_WEIGHT_DIV - 1);
        return (double)td->max_value - t * ((double)td->max_value - last->mean);
    }

    uint64_t current_weight = first->weight / TDIGEST_HALF_WEIGHT_DIV;
    for (size_t i = 0; i + 1 < active->size; i++) {
        const tdigest_centroid_t *left = &active->centroids[i];
        const tdigest_centroid_t *right = &active->centroids[i + 1];
        uint64_t segment_weight = (left->weight + right->weight) / TDIGEST_HALF_WEIGHT_DIV;
        if (current_weight + segment_weight > index) {
            uint64_t lower = index - current_weight;
            uint64_t upper = current_weight + segment_weight - index;
            return (left->mean * (double)upper + right->mean * (double)lower) / (double)(lower + upper);
        }
        current_weight += segment_weight;
    }
    return active->centroids[active->size - 1].mean;
}

void tdigest_merge(tdigest_t *dst, tdigest_t *src)
{
    if (dst == NULL || src == NULL || src->active == NULL) {
        return;
    }
    tdigest_flush(src);
    if (src->active->size == 0) {
        return;
    }
    for (size_t i = 0; i < src->active->size; i++) {
        tdigest_insert(dst, src->active->centroids[i].mean, src->active->centroids[i].weight);
    }
    if (src->min_value < dst->min_value) {
        dst->min_value = src->min_value;
    }
    if (src->max_value > dst->max_value) {
        dst->max_value = src->max_value;
    }
}
