#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYS_LOG_INFO = 0,
    SYS_LOG_WARN = 1,
    SYS_LOG_ERR  = 2,
} sys_log_level_t;

typedef struct {
    int cpu_pct;
    int ram_pct;
    uint32_t ram_used_kb;
    uint32_t ram_total_kb;
    int psram_pct;
    int storage_pct;
    uint32_t storage_used_kb;
    uint32_t storage_total_kb;
    uint32_t uptime_sec;
    uint32_t err_count;
    uint32_t warn_count;
} sys_metrics_t;

void sys_log_init(void);
void sys_log_add(sys_log_level_t level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void sys_log_clear(void);
void sys_log_get_metrics(sys_metrics_t *out);
void sys_log_to_json(char *out_json, size_t out_sz);

#ifdef __cplusplus
}
#endif
