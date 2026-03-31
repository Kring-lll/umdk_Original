#ifndef URMA_PERF_H
#define URMA_PERF_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum urma_perf_type {
    UB_JETTY_POST_SEND,
    BOND_JETTY_POST_SEND,
    UB_JFS_POST_SEND,
    BOND_JFS_POST_SEND,
    UB_JETTY_POST_RECV,
    BOND_JETTY_POST_RECV,
    UB_POST_JFR_RECV,
    BOND_POST_JFR_RECV,
    UB_POLL_JFC,
    BOND_POLL_JFC,
    UB_WAIT_JFC,
    BOND_WAIT_JFC,
    UB_ACK_JFC,
    UB_REARM_JFC,
    BOND_REARM_JFC,
    URMA_PERF_RECORD_TYPE_MAX,
} urma_perf_record_type_t;

typedef struct UrmaPerfContext {
    void* histograms[URMA_PERF_RECORD_TYPE_MAX];
    void* stats;
    int32_t is_running;
} UrmaPerfContext;

extern __thread UrmaPerfContext* g_perf_ctx;

int32_t urma_start_perf(void);
int32_t urma_stop_perf(void);
void urma_step_perf(urma_perf_record_type_t type, uint64_t latency);
int32_t urma_get_perf_info(char *perf_buf, uint32_t *length);
void urma_perf_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif
