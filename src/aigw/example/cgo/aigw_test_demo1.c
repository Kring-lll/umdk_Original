/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 *
 * @file example_aigw.c
 * @brief Example application demonstrating the usage of AIGW (AI Gateway) C API.
 *
 * This example illustrates how to initialize the AIGW component, register a cache driver,
 * perform node selection for inference requests using load balancing, notify events,
 * and manage concurrent AI inference workloads.
 *
 * It spawns 10 concurrent threads to simulate real-time inference traffic, showcasing
 * thread safety and performance characteristics of the AIGW API under load.
 *
 * The example uses mock implementations for cache operations to avoid external dependencies,
 * making it self-contained and suitable for integration testing or development reference.
 *
 * @note This example assumes that libaigw.so is properly installed and linked.
 *       The mock cache driver demonstrates the expected behavior of real drivers
 *       (e.g., Redis, DCS) without requiring network services.
 *
 *       All string buffers are safely handled within defined length limits.
 *       Dynamic memory is carefully managed to prevent leaks.
 *
 * @create 2026-01-26
 * @modify 2026-03-23 - Added 7x24 continuous running capability.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>
#include <signal.h>
#include "aigw.h"
#include "simple_cache.h"

#ifdef ENABLE_REDIS_DRIVER
#include "redis_cache_driver.h"
#endif

// #define NUM_OF_REQUEST 200
#define NUM_OF_THREADS 200           // 并发线程数
#define NUM_OF_REQUEST 200          // 原请求次数（已弃用）
#define MODEL_NAME "qwen-72b"

#define NUM_OF_MAP_FIELDS 2

#define AIGW_UUID_MAX_LEN 36
#define AIGW_ROLE_MAX_LEN 32
#define AIGW_MODEL_STR_MAX_LEN 64
#define AIGW_GENERIC_STR_MAX_LEN 128
#define AIGW_LOG_LEVEL_MAX_LEN 16
#define AIGW_LOG_PATH_MAX_LEN 256
#define MODEL_COUNT 1024
#define NODE_COUNT (1024)
#define GROUP_SIZE 8
#define CONTENT_SIZE (10 * 1024 * 1024)

// 全局运行标志，用于控制线程是否继续执行
static volatile sig_atomic_t running = 1;

// 信号处理函数，用于优雅退出
static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

// Utility function: generate a simple UUID string (for example purposes only)
static void generate_uuid(char *buf, size_t len, long long id)
{
    snprintf(buf, len, "req-%010lld", id);
}

// Utility function: safely copy string into fixed-size buffer
static void safe_strcpy(char *dst, const char *src, size_t max_len)
{
    if (max_len > 0) {
        strncpy(dst, src, max_len - 1);
        dst[max_len - 1] = '\0';
    } else {
        *dst = '\0';
    }
}

static char *g_model[MODEL_COUNT];

static void init_models(void)
{
    for (int i = 0; i < MODEL_COUNT; ++i) {
        g_model[i] = (char *)malloc(20);
        if (g_model[i] == NULL) {
            fprintf(stderr, "Failed to allocate memory for g_model[%d]\n", i);
            exit(EXIT_FAILURE);
        }
        snprintf(g_model[i], 20, "qwen-%d", i);
    }
}

static aigw_node_info_t *g_nodes = NULL;

static int init_nodes(void)
{
    g_nodes = (aigw_node_info_t *)malloc(sizeof(aigw_node_info_t) * NODE_COUNT);
    if (!g_nodes) {
        return -1;
    }

    int i;
    for (i = 0; i < NODE_COUNT; ++i) {
        aigw_node_info_t *node = &g_nodes[i];
        node->role = AIGW_INFER_PREFILL;

        // 分配 node_addr 缓冲区，大小为 32 字节足够容纳 "192.168.xxx.xxx:8080"
        char *addr_buf = (char *)malloc(32);
        if (!addr_buf) {
            goto cleanup;
        }
        snprintf(addr_buf, 32, "192.168.%zu.%zu:8080", (i / 256) % 256, i % 256);
        node->node_addr = addr_buf;

        // 分配 group_id 缓冲区，大小为 32 字节足够容纳 "group-xxxxxx", group id 每 8 个相同
        char *group_buf = (char *)malloc(32);
        if (!group_buf) {
            free(addr_buf);
            goto cleanup;
        }
        size_t group_idx = i / GROUP_SIZE;
        snprintf(group_buf, 32, "group-%zu", group_idx);
        node->group_id = group_buf;
    }
    // 打印所有节点信息
    printf("Initialized %d nodes:\n", NODE_COUNT);
    for (i = 0; i < NODE_COUNT; ++i) {
        printf("  node[%d]: addr=%s, group=%s, role=%d\n",
               i, g_nodes[i].node_addr, g_nodes[i].group_id, g_nodes[i].role);
    }
    return 0;

cleanup:
    // 释放已成功分配的所有节点字符串和数组
    for (int j = 0; j < i; ++j) {
        free((void *)g_nodes[j].node_addr);
        free((void *)g_nodes[j].group_id);
    }
    free(g_nodes);
    g_nodes = NULL;
    return -1;
}

static char *g_buffer = NULL;   // 全局只读内容缓冲区，所有线程共享

static const char *default_pretrain_ttft_filepath = "/tmp/aigw/example/ttft_pretrain.txt";

// 线程函数：持续处理请求
static void* process_requests(void *arg)
{
    int thread_id = *(int*)arg;
    free(arg); // 释放传入的线程ID

    // 线程本地请求计数器，用于生成唯一 UUID
    long long local_req_id = 0;

    // 每次请求使用的消息结构（内容指针指向全局只读缓冲区）
    aigw_openai_message_t message;
    message.role = "user";
    message.content = g_buffer;   // 所有线程共享同一个只读内容

    // 请求上下文（每次请求可复用）
    aigw_select_context_t ctx = {
        .node_num = NODE_COUNT,
        .node_list = g_nodes
    };

    // 每次请求的结果结构
    aigw_select_result_t result = {0};

    while (running) {
        char uuid[AIGW_UUID_MAX_LEN];
        generate_uuid(uuid, sizeof(uuid), local_req_id++);

        // 构造请求
        aigw_request_t request = {0};
        request.uuid = uuid;
        request.model = g_model[thread_id % MODEL_COUNT];  // 每个线程使用不同模型（轮询）
        request.messages = &message;
        request.message_num = 1;

        // 模拟请求前的延时（避免瞬时压力过大）
        int sleep_us1 = (rand() % 1000 + 1000) * 1000;  // 1s ~ 2s
        printf("[Thread %d] Request %s: Sleeping %d ms before node selection...\n",
               thread_id, uuid, sleep_us1 / 1000);
        usleep(sleep_us1);

        clock_t start = clock();
        aigw_error_t err = aigw_select_nodes(&request, &ctx, &result);
        if (err != AIGW_SUCCESS) {
            fprintf(stderr, "[Thread %d] Request %s: Node selection failed: %d, error: %s\n",
                    thread_id, uuid, err, result.error_desc);
            // 出错后短暂休眠再继续
            usleep(1000000);
            continue;
        }
        clock_t end = clock();

        printf("[Thread %d] Request %s: Prefill=%s, Decode=%s, cost %f ms\n",
               thread_id, uuid, result.prefill_node_addr, result.decode_node_addr,
               (double)(end - start) * 1000 / CLOCKS_PER_SEC);

        // 模拟 prefill 完成后的延迟
        int sleep_us2 = (rand() % 1000 + 6000) * 1000;  // 6s ~ 7s
        printf("[Thread %d] Request %s: Sleeping %d ms before notifying DECODE_RECEIVED_KVC\n",
               thread_id, uuid, sleep_us2 / 1000);
        usleep(sleep_us2);

        // 通知事件：prefill 完成，解码器已接收 KV cache
        aigw_event_info_t event = {0};
        event.model = request.model;
        event.request_id = request.uuid;
        event.event_name = "DECODE_RECEIVED_KVC";
        aigw_notify_event(AIGW_EVENT_REQUEST, &event);

        // 模拟解码完成前的延迟
        int sleep_us3 = (rand() % 1000 + 1000) * 1000;  // 1s ~ 2s
        printf("[Thread %d] Request %s: Sleeping %d ms before notifying REQUEST_IS_FINISHED\n",
               thread_id, uuid, sleep_us3 / 1000);
        usleep(sleep_us3);

        // 通知事件：请求完成
        event.event_name = "REQUEST_IS_FINISHED";
        aigw_notify_event(AIGW_EVENT_REQUEST, &event);
    }

    printf("[Thread %d] Exiting.\n", thread_id);
    return NULL;
}

int main(int argc, char *argv[])
{
    printf("=== AIGW API Example: %d Concurrent Threads, 7x24 Running ===\n", NUM_OF_THREADS);

    // 安装信号处理函数，以便优雅退出
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    srand((unsigned int)time(NULL));

    // 分配全局内容缓冲区（只读）
    g_buffer = (char *)malloc(CONTENT_SIZE + 1);
    if (!g_buffer) {
        fprintf(stderr, "Failed to allocate content buffer\n");
        return -1;
    }
    memset(g_buffer, 'A', CONTENT_SIZE);
    g_buffer[CONTENT_SIZE] = '\0';

    // 初始化模型数组
    init_models();

    // 初始化节点列表
    if (init_nodes() != 0) {
        fprintf(stderr, "init_nodes failed\n");
        free(g_buffer);
        return -1;
    }

    // 1. 初始化组件配置
    aigw_config_t cfg = {0};
    cfg.log_level = "debug";
    cfg.log_path = "/tmp";
    cfg.max_instances_per_model = 64 * 1024;
    cfg.max_supported_models = 64 * 1024;
    cfg.max_prompt_length = 16 * 1024 * 1024;
    cfg.request_ttl_seconds = 600;

    printf("Initializing AIGW...\n");
    aigw_error_t err = aigw_init(&cfg);
    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "aigw_init failed with error: %d\n", err);
        free(g_buffer);
        free(g_nodes);
        for (int i = 0; i < MODEL_COUNT; ++i) free(g_model[i]);
        return -1;
    }

    // 2. 注册缓存驱动
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

    // 3. 测试缓存操作
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

    // 4. 注册所有模型
    aigw_model_config_t model_cfg = {
        .deploy_policy = AIGW_DEPLOY_SEPERATED,
        .p_lb_type = AIGW_LB_PREFILL_TIME_AWARE,
        .d_lb_type = AIGW_LB_NONE,
        .pretrain_ttft_path = default_pretrain_ttft_filepath,
        .tokenization_ratio = 0.5,
    };
    for (int i = 0; i < MODEL_COUNT; ++i) {
        model_cfg.model = g_model[i];
        int ret = aigw_register_model(&model_cfg);
        if (ret != AIGW_SUCCESS) {
            fprintf(stderr, "aigw_register_model failed for %s: %d\n", g_model[i], ret);
        }
    }

    // 5. 创建多个线程，每个线程持续处理请求
    printf("Spawning %d concurrent threads for continuous request processing...\n", NUM_OF_THREADS);
    pthread_t threads[NUM_OF_THREADS];
    int thread_created[NUM_OF_THREADS] = {0};

    for (int i = 0; i < NUM_OF_THREADS; i++) {
        int *id = (int *)malloc(sizeof(int));
        if (id == NULL) {
            fprintf(stderr, "Failed to allocate memory for thread id %d\n", i);
            continue;
        }
        *id = i;
        int ret = pthread_create(&threads[i], NULL, process_requests, id);
        if (ret != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            free(id);
            thread_created[i] = 0;
        } else {
            thread_created[i] = 1;
        }
    }

    // 主线程等待 7×24 小时或直到收到退出信号
    printf("Running for 7x24 hours... Press Ctrl+C to stop early.\n");
    int hours = 7 * 24;  // 7 days
    int seconds = hours * 3600;
    while (running && seconds > 0) {
        int sleep_sec = (seconds < 3600) ? seconds : 3600;
        sleep(sleep_sec);
        seconds -= sleep_sec;
    }

    // 若达到预定时间，则设置退出标志
    if (running) {
        printf("7x24 hours elapsed, stopping...\n");
        running = 0;
    } else {
        printf("Signal received, stopping...\n");
    }

    // 等待所有线程结束
    for (int i = 0; i < NUM_OF_THREADS; i++) {
        if (!thread_created[i]) continue;
        pthread_join(threads[i], NULL);
    }

    // 6. 注销所有模型
    for (int i = 0; i < MODEL_COUNT; ++i) {
        err = aigw_unregister_model(g_model[i]);
        if (err != AIGW_SUCCESS) {
            fprintf(stderr, "aigw_unregister_model failed for %s: %d\n", g_model[i], err);
        }
    }

    // 7. 注销缓存驱动
    printf("Unregistering cache driver...\n");
    err = aigw_unregister_cache_driver();
    if (err != AIGW_SUCCESS) {
        fprintf(stderr, "aigw_unregister_cache_driver failed: %d\n", err);
    }

cleanup:
    // 8. 清理并退出
    printf("Uninitializing AIGW...\n");
    aigw_uninit();

    // 释放全局资源
    for (int i = 0; i < MODEL_COUNT; ++i) {
        free(g_model[i]);
    }
    free(g_nodes);
    free(g_buffer);
    g_buffer = NULL;

    printf("Example completed.\n");
    return 0;
}