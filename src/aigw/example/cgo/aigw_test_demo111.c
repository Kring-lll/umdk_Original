#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include "aigw.h"
#include "simple_cache.h"

#ifdef ENABLE_REDIS_DRIVER
#include "redis_cache_driver.h"
#endif

// ==================== Configurable Parameters ====================
#define NUM_OF_REQUEST        1000          // Total number of requests
#define NUM_MODELS            1            // Number of models
#define NUM_INSTANCES         200            // Number of node instances per model

#define AIGW_UUID_MAX_LEN     36
#define AIGW_MODEL_STR_MAX_LEN 64
#define AIGW_CONTENT_POOL_SIZE 10

// ==================== Global Variables (Dynamically Allocated) ====================
static char** g_model_names = NULL;
static aigw_node_info_t* g_nodes = NULL;
static int g_node_count = 0;
static pid_t g_process_id;

static const char* g_contents[AIGW_CONTENT_POOL_SIZE] = {
    "Hello, how are you?",
    "Tell me about AI.",
    "Write a poem about spring.",
    "Explain quantum computing.",
    "Translate 'hello' to French.",
    "Summarize the last meeting.",
    "Generate Python code for Fibonacci.",
    "What is the weather today?",
    "Recommend a good book.",
    "Debug this C code snippet."
};
static void generate_uuid(char *buf, size_t len, int seq_id)
{
    pid_t pid = g_process_id;
    time_t now = time(NULL);
    snprintf(buf, len, "req-%d-%ld-%06d", pid, now, seq_id);
}

static void init_model_names()
{
    g_model_names = calloc(NUM_MODELS, sizeof(char*));
    if (!g_model_names) {
        perror("calloc failed for g_model_names");
        exit(1);
    }

    for (int i = 0; i < NUM_MODELS; i++) {
        g_model_names[i] = calloc(AIGW_MODEL_STR_MAX_LEN, 1);
        if (!g_model_names[i]) {
            perror("calloc failed for model name");
            exit(1);
        }
        snprintf(g_model_names[i], AIGW_MODEL_STR_MAX_LEN, "model-%03d", i + 1);
        printf("ytt get modelname %s\n", g_model_names[i]);
    }
}

static void init_node_list()
{
    g_nodes = calloc(NUM_INSTANCES, sizeof(aigw_node_info_t));
    if (!g_nodes) {
        perror("calloc failed");
        exit(1);
    }

    g_node_count = NUM_INSTANCES;

    for (int i = 0; i < NUM_INSTANCES; i++) {
        g_nodes[i].role = (i % 2 == 0) ? AIGW_INFER_PREFILL : AIGW_INFER_DECODE;

        char* addr = malloc(32);
        if (!addr) {
            exit(1);
        }
        int a = i / 254;
        int b = i % 254 + 1;
        snprintf(addr, 32, "10.0.%d.%d:8080", a, b);
        printf("ytt get %s\n", addr);
        g_nodes[i].node_addr = addr;
        g_nodes[i].group_id = "group-a";
    }
}

static void cleanup_globals()
{
    if (g_model_names) {
        for (int i = 0; i < NUM_MODELS; i++) {
            free(g_model_names[i]);
        }
        free(g_model_names);
        g_model_names = NULL;
    }

    if (g_nodes) {
        for (int i = 0; i < g_node_count; i++) {
            free((void*)g_nodes[i].node_addr);
        }
        free(g_nodes);
        g_nodes = NULL;
    }
}

static void* process_request(void *arg)
{
    int req_id = *(int*)arg;
    free(arg);

    char uuid[AIGW_UUID_MAX_LEN];
    generate_uuid(uuid, sizeof(uuid), req_id);

    int model_idx = req_id % NUM_MODELS;
    const char* selected_model = g_model_names[model_idx];

    aigw_openai_message_t message;
    message.role = "user";
    message.content = g_contents[req_id % AIGW_CONTENT_POOL_SIZE];

    aigw_request_t request = {0};
    request.uuid = uuid;
    request.model = selected_model;
    request.messages = &message;
    request.message_num = 1;

    aigw_select_context_t ctx = {
        .node_num = g_node_count,
        .node_list = g_nodes
    };

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    aigw_select_result_t result = {0};
    aigw_error_t err = aigw_select_nodes(&request, &ctx, &result);

    clock_gettime(CLOCK_MONOTONIC, &end);

    long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL +
                           (end.tv_nsec - start.tv_nsec) / 1000;
    double elapsed_ms = elapsed_us / 1000.0;

    printf("Request %s: aigw_select_nodes took %lld us\n", uuid, elapsed_us);

    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "Request %s: Node selection failed: %d, error: %s\n",
                uuid, err, result.error_desc ? result.error_desc : "Unknown");
        return NULL;
    }

    printf("Request %s: Prefill=%s, Decode=%s\n",
           uuid, result.prefill_node_addr, result.decode_node_addr);

    // Notify DECODE_RECEIVED_KVC after random delay (100~600ms)
    aigw_event_info_t event = {0};
    event.model = request.model;
    event.request_id = request.uuid;
    event.event_name = "DECODE_RECEIVED_KVC";

    int delay1 = (rand() % 1000 + 500); // 0.5 ~ 1.5s
//    int delay1 = 5000;
    usleep(delay1 * 1000);
    int yttresult = aigw_notify_event(AIGW_EVENT_REQUEST, &event);

    // Notify REQUEST_IS_FINISHED after another delay (1000~2000ms after KVC)
    event.event_name = "REQUEST_IS_FINISHED";
//    event.request_id = "ytt1";
//    int delay2 = (rand() % 28000 + 2000); // 2 ~ 30 s
    int delay2 = (rand() % 1000 + 2000); // 2 ~ 30 s
    usleep(delay2 * 1000);
//    int result1 = aigw_notify_event(AIGW_EVENT_REQUEST, &event);
//    printf("ytt get res1:%d  res2:%d\n",yttresult,result1);

    return NULL;
}

int main(int argc, char *argv[])
{
    printf("=== AIGW API Example: %d Requests, %d Models, %d Instances ===\n",
           NUM_OF_REQUEST, NUM_MODELS, NUM_INSTANCES);
    g_process_id = getpid();
    srand((unsigned int)time(NULL) ^ g_process_id);

    init_model_names();
    init_node_list();

    aigw_config_t cfg = {0};
    cfg.log_level = "debug";
    cfg.log_path = "/tmp";
    cfg.max_instances_per_model = 512;
    cfg.max_supported_models = 512;
    cfg.max_prompt_length = 20480;
    cfg.request_ttl_seconds = 600;

    printf("Initializing AIGW...\n");
    aigw_error_t err = aigw_init(&cfg);
    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "aigw_init failed with error: %d\n", err);
        goto cleanup;
    }

    printf("Registering cache driver...\n");
#ifdef ENABLE_REDIS_DRIVER
    aigw_cache_driver_t *driver = get_redis_cache_driver();
#else
    aigw_cache_driver_t *driver = get_simple_cache_driver();
#endif

    err = aigw_register_cache_driver(driver);
    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "aigw_register_cache_driver failed: %d\n", err);
        goto cleanup;
    }

    printf("Testing cache operations...\n");
#ifdef ENABLE_REDIS_DRIVER
    err = test_redis_cache();
#else
    err = test_simple_cache();
#endif

    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "test_simple_cache failed: %d\n", err);
        goto cleanup;
    }

    // Launch requests at rate: 20 requests per second (50ms interval)
    const int REQUEST_RATE_PER_SEC = 500;
    const int INTERVAL_US = 1000000 / REQUEST_RATE_PER_SEC; // 50,000 us = 50ms

    printf("Spawning %d requests at %d per second...\n", NUM_OF_REQUEST, REQUEST_RATE_PER_SEC);

    pthread_t threads[NUM_OF_REQUEST];
    for (int i = 0; i < NUM_OF_REQUEST; i++) {
        int *id = malloc(sizeof(int));
        if (!id) {
            perror("malloc failed for thread id");
            continue;
        }
        *id = i + 1;

        int ret = pthread_create(&threads[i], NULL, process_request, id);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            free(id);
            threads[i] = 0; // mark invalid
        }

        // Control rate: sleep after each submission (except last)
        if (i < NUM_OF_REQUEST - 1) {
            usleep(INTERVAL_US);
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < NUM_OF_REQUEST; i++) {
        if (threads[i] == 0) continue;
        pthread_join(threads[i], NULL);
    }

    printf("Unregistering cache driver...\n");
    err = aigw_unregister_cache_driver();
    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "aigw_unregister_cache_driver failed: %d\n", err);
    }

cleanup:
    printf("Uninitializing AIGW...\n");
    aigw_uninit();

    cleanup_globals();

    printf("Example completed.\n");
    return 0;
}
