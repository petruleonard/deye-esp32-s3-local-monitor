#include "deye_data.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static deye_data_t s_telemetry = {
    .pv_power_w = 0,
    .pv1_power_w = 0,
    .pv2_power_w = 0,
    .pv1_volts = 0.0f,
    .pv2_volts = 0.0f,
    .pv_today_kwh = 32.4f,
    .pv_total_kwh = 189.2f,
    .battery_soc = 99,
    .battery_soh = 99,
    .battery_volts = 51.2f,
    .battery_current = 14.4f,
    .battery_temp_c = 24.5f,
    .battery_capacity_kwh = 30.0f,
    .battery_charge_today_kwh = 9.4f,
    .battery_discharge_today_kwh = 4.8f,
    .grid_hz = 50.0f,
    .grid_volts = 236.8f,
    .grid_l2_volts = 240.9f,
    .grid_l3_volts = 239.6f,
    .grid_sell_today_kwh = 18.2f,
    .grid_buy_today_kwh = 2.1f,
    .load_today_kwh = 11.4f,
    .load_pf = 0.98f,
    .load_peak_today_kw = 3.2f,
    .load_avg_hourly_kw = 0.53f,
    .load_source_str = "Solar",
    .inverter_status = 2, // Normal
    .inverter_temp_c = 35.0f,
    .wifi_connected = false,
    .logger_connected = false,
    .status_str = "Inițializare..."
};

static SemaphoreHandle_t s_data_mutex = NULL;

void deye_data_init(void)
{
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
    }
}

void deye_data_get(deye_data_t *out)
{
    if (!out) return;
    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        memcpy(out, &s_telemetry, sizeof(deye_data_t));
        xSemaphoreGive(s_data_mutex);
    } else {
        memcpy(out, &s_telemetry, sizeof(deye_data_t));
    }
}

void deye_data_update(const deye_data_t *in)
{
    if (!in) return;
    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        memcpy(&s_telemetry, in, sizeof(deye_data_t));
        xSemaphoreGive(s_data_mutex);
    } else {
        memcpy(&s_telemetry, in, sizeof(deye_data_t));
    }
}

