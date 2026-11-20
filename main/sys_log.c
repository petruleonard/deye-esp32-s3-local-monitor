#include "sys_log.h"
#include "history_store.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define SYS_LOG_CAP 80

typedef struct {
    uint32_t seq;
    uint32_t uptime_sec;
    char ts_str[12]; // HH:MM:SS or +00:12:34
    uint8_t level;
    char tag[14];
    char msg[112];
} sys_log_entry_t;

static sys_log_entry_t s_entries[SYS_LOG_CAP];
static int s_head = 0;
static int s_count = 0;
static uint32_t s_seq = 0;
static uint32_t s_err_count = 0;
static uint32_t s_warn_count = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

void sys_log_init(void)
{
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
}

static void format_timestamp(char *out, size_t out_sz, uint32_t uptime_sec)
{
    time_t now = 0;
    time(&now);
    struct tm tmi;
    localtime_r(&now, &tmi);
    if (tmi.tm_year + 1900 >= 2024) {
        snprintf(out, out_sz, "%02d:%02d:%02d", tmi.tm_hour, tmi.tm_min, tmi.tm_sec);
    } else {
        uint32_t h = uptime_sec / 3600;
        uint32_t m = (uptime_sec % 3600) / 60;
        uint32_t s = uptime_sec % 60;
        snprintf(out, out_sz, "+%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

void sys_log_add(sys_log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!s_log_mutex) {
        sys_log_init();
        if (!s_log_mutex) return;
    }

    char formatted[112];
    va_list args;
    va_start(args, fmt);
    vsnprintf(formatted, sizeof(formatted), fmt, args);
    va_end(args);

    uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char ts[12];
    format_timestamp(ts, sizeof(ts), uptime_sec);

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    s_seq++;
    sys_log_entry_t *e = &s_entries[s_head];
    e->seq = s_seq;
    e->uptime_sec = uptime_sec;
    snprintf(e->ts_str, sizeof(e->ts_str), "%s", ts);
    e->level = (uint8_t)level;
    snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "sys");
    snprintf(e->msg, sizeof(e->msg), "%s", formatted);

    s_head = (s_head + 1) % SYS_LOG_CAP;
    if (s_count < SYS_LOG_CAP) s_count++;

    if (level == SYS_LOG_ERR) s_err_count++;
    else if (level == SYS_LOG_WARN) s_warn_count++;

    xSemaphoreGive(s_log_mutex);
}

void sys_log_clear(void)
{
    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_head = 0;
    s_count = 0;
    s_err_count = 0;
    s_warn_count = 0;
    xSemaphoreGive(s_log_mutex);
    sys_log_add(SYS_LOG_INFO, "sys", "Jurnalul de sistem a fost sters.");
}

static int compute_cpu_load_pct(void)
{
    static int s_last_cpu_pct = 0;
    static int64_t s_last_sample_us = 0;
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    static bool s_has_prev = false;
    static uint32_t s_prev_total = 0;
    static uint32_t s_prev_idle = 0;

    int64_t now_us = esp_timer_get_time();
    if (s_has_prev && (now_us - s_last_sample_us) < 1500000LL) {
        return s_last_cpu_pct;
    }

    UBaseType_t n_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = malloc((size_t)(n_tasks + 4) * sizeof(TaskStatus_t));
    if (!arr) return s_last_cpu_pct;

    uint32_t total_rt = 0;
    UBaseType_t got = uxTaskGetSystemState(arr, n_tasks + 4, &total_rt);
    uint32_t idle_rt = 0;
    for (UBaseType_t i = 0; i < got; i++) {
        if (arr[i].pcTaskName && strncmp(arr[i].pcTaskName, "IDLE", 4) == 0) {
            idle_rt += (uint32_t)arr[i].ulRunTimeCounter;
        }
    }
    free(arr);

    if (s_has_prev) {
        uint32_t dt = total_rt - s_prev_total;
        uint32_t d_idle = idle_rt - s_prev_idle;
        uint64_t cap = (uint64_t)dt * (uint64_t)portNUM_PROCESSORS;
        if (cap > 0) {
            int64_t pct = 100LL - (((int64_t)d_idle * 100LL) / (int64_t)cap);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            s_last_cpu_pct = (int)pct;
        }
    }

    s_prev_total = total_rt;
    s_prev_idle = idle_rt;
    s_last_sample_us = now_us;
    s_has_prev = true;
#else
    (void)s_last_sample_us;
#endif
    return s_last_cpu_pct;
}

void sys_log_get_metrics(sys_metrics_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->cpu_pct = compute_cpu_load_pct();
    out->uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->ram_total_kb = (uint32_t)(int_total / 1024U);
    out->ram_used_kb  = (int_total > int_free) ? (uint32_t)((int_total - int_free) / 1024U) : 0;
    out->ram_pct = (int_total > 0 && int_total >= int_free)
                   ? (int)(((int_total - int_free) * 100ULL) / int_total)
                   : 0;

    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_pct = (ps_total > 0 && ps_total >= ps_free)
                     ? (int)(((ps_total - ps_free) * 100ULL) / ps_total)
                     : 0;

    size_t st_total = 0, st_used = 0;
    if (history_store_get_storage_info(&st_total, &st_used) && st_total > 0) {
        out->storage_total_kb = (uint32_t)(st_total / 1024U);
        out->storage_used_kb  = (uint32_t)(st_used / 1024U);
        out->storage_pct = (int)((st_used * 100ULL) / st_total);
    }

    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        out->err_count = s_err_count;
        out->warn_count = s_warn_count;
        xSemaphoreGive(s_log_mutex);
    }
}

static void esc_json_str(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] != '\0' && j + 2 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

void sys_log_to_json(char *out_json, size_t out_sz)
{
    if (!out_json || out_sz < 64) return;

    sys_metrics_t m;
    sys_log_get_metrics(&m);

    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(150)) != pdTRUE) {
        snprintf(out_json, out_sz, "{\"entries\":[]}");
        return;
    }

    size_t pos = snprintf(out_json, out_sz,
        "{\"cpu_pct\":%d,\"ram_pct\":%d,\"ram_used_kb\":%lu,\"ram_total_kb\":%lu,"
        "\"psram_pct\":%d,\"storage_pct\":%d,\"storage_used_kb\":%lu,\"storage_total_kb\":%lu,"
        "\"uptime_sec\":%lu,\"err_count\":%lu,\"warn_count\":%lu,\"entries\":[",
        m.cpu_pct, m.ram_pct, (unsigned long)m.ram_used_kb, (unsigned long)m.ram_total_kb,
        m.psram_pct, m.storage_pct, (unsigned long)m.storage_used_kb, (unsigned long)m.storage_total_kb,
        (unsigned long)m.uptime_sec, (unsigned long)s_err_count, (unsigned long)s_warn_count);

    // Output newest entries first for fast reading
    bool first = true;
    for (int i = 0; i < s_count; i++) {
        int idx = (s_head - 1 - i + SYS_LOG_CAP) % SYS_LOG_CAP;
        const sys_log_entry_t *e = &s_entries[idx];
        const char *lvl = (e->level == SYS_LOG_ERR) ? "ERR" : (e->level == SYS_LOG_WARN ? "WARN" : "INFO");
        char esc_msg[224], esc_tag[32];
        esc_json_str(e->msg, esc_msg, sizeof(esc_msg));
        esc_json_str(e->tag, esc_tag, sizeof(esc_tag));
        if (pos + 280 >= out_sz) break;
        pos += snprintf(out_json + pos, out_sz - pos,
                        "%s{\"seq\":%lu,\"ts\":\"%s\",\"lvl\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
                        first ? "" : ",",
                        (unsigned long)e->seq, e->ts_str, lvl, esc_tag, esc_msg);
        first = false;
    }

    snprintf(out_json + pos, out_sz - pos, "]}");
    xSemaphoreGive(s_log_mutex);
}
