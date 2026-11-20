#include "sys_metrics.h"
#include "history_store.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_timer.h"

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

static uint64_t get_static_flash_used_bytes(uint64_t *out_flash_total_bytes)
{
    static bool s_cached = false;
    static uint64_t s_flash_total = 0;
    static uint64_t s_static_used = 0;

    if (!s_cached) {
        uint32_t chip_sz = 0;
        if (esp_flash_get_size(NULL, &chip_sz) == ESP_OK && chip_sz > 0) {
            s_flash_total = (uint64_t)chip_sz;
        } else {
            s_flash_total = 16ULL * 1024ULL * 1024ULL;
        }

        // Bootloader + Partition Table + NVS + OTAdata + PHY fixed footprint up to 0x20000 (~128 KB max, ~72 KB active)
        uint64_t used = 72ULL * 1024ULL;

        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it != NULL) {
            const esp_partition_t *part = esp_partition_get(it);
            if (part) {
                uint8_t magic = 0;
                if (esp_partition_read(part, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
                    esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
                    esp_image_metadata_t meta;
                    if (esp_image_get_metadata(&pos, &meta) == ESP_OK && meta.image_len > 0) {
                        used += (uint64_t)meta.image_len;
                    }
                }
            }
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);

        s_static_used = used;
        s_cached = true;
    }

    if (out_flash_total_bytes) *out_flash_total_bytes = s_flash_total;
    return s_static_used;
}

void sys_metrics_get(sys_metrics_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->cpu_pct = compute_cpu_load_pct();

    size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->ram_total_kb = (uint32_t)(int_total / 1024U);
    out->ram_used_kb  = (int_total > int_free) ? (uint32_t)((int_total - int_free) / 1024U) : 0;
    out->ram_pct = (int_total > 0 && int_total >= int_free)
                   ? (int)(((int_total - int_free) * 100ULL + (int_total / 2U)) / int_total)
                   : 0;

    size_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_pct = (ps_total > 0 && ps_total >= ps_free)
                     ? (int)(((ps_total - ps_free) * 100ULL + (ps_total / 2U)) / ps_total)
                     : 0;

    uint64_t flash_total_bytes = 0;
    uint64_t flash_used_bytes = get_static_flash_used_bytes(&flash_total_bytes);

    size_t st_total = 0, st_used = 0;
    if (history_store_get_storage_info(&st_total, &st_used)) {
        out->spiffs_total_kb = (uint32_t)(st_total / 1024U);
        out->spiffs_used_kb  = (uint32_t)(st_used / 1024U);
        flash_used_bytes += (uint64_t)st_used;
    }

    if (flash_used_bytes > flash_total_bytes) flash_used_bytes = flash_total_bytes;
    out->storage_total_kb = (uint32_t)(flash_total_bytes / 1024ULL);
    out->storage_used_kb  = (uint32_t)(flash_used_bytes / 1024ULL);
    out->storage_pct = (flash_total_bytes > 0)
                       ? (int)((flash_used_bytes * 100ULL + (flash_total_bytes / 2ULL)) / flash_total_bytes)
                       : 0;
    if (out->storage_pct < 1 && flash_used_bytes > 0) out->storage_pct = 1;
}
