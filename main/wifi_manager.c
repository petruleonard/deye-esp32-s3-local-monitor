#include "wifi_manager.h"
#include "app_settings.h"
#include "app_config.h"
#include "sys_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_is_connected = false;
static bool s_ap_active = false;
static char s_ip_str[16] = "0.0.0.0";

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Connection is triggered explicitly by wifi_manager_connect() after setting config.
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = dis ? dis->reason : 0;
        s_is_connected = false;
        strcpy(s_ip_str, "0.0.0.0");
        sys_log_add(SYS_LOG_WARN, "wifi", "Wi-Fi deconectat (motiv=%u)", (unsigned)reason);
        if (!s_ap_active && strlen(g_settings.wifi_ssid) > 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u). Reconnecting in 3s...", (unsigned)reason);
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Wi-Fi connected! Got IP: %s", s_ip_str);
        sys_log_add(SYS_LOG_INFO, "wifi", "Wi-Fi conectat, IP: %s", s_ip_str);
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        s_sta_netif = esp_netif_create_default_wifi_sta();
        s_ap_netif = esp_netif_create_default_wifi_ap();
        (void)s_sta_netif;
        (void)s_ap_netif;

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_got_ip));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    wifi_manager_connect();
    return ESP_OK;
}

void wifi_manager_connect(void)
{
    if (strlen(g_settings.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty, skipping STA connection.");
        return;
    }

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, g_settings.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_settings.wifi_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (strlen(g_settings.wifi_password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "Setting Wi-Fi configuration for SSID: %s", g_settings.wifi_ssid);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_wifi_connect();
}

esp_err_t wifi_manager_start_ap_portal(void)
{
    if (s_ap_active) {
        return ESP_OK;
    }
    s_ap_active = true;
    esp_wifi_disconnect();

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, BRIDGE_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(BRIDGE_AP_SSID);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGI(TAG, "Wi-Fi Config Portal SoftAP started: SSID='%s' -> http://192.168.4.1", BRIDGE_AP_SSID);
    return ESP_OK;
}

bool wifi_manager_is_ap_active(void)
{
    return s_ap_active;
}

bool wifi_manager_test_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || strlen(ssid) == 0) return false;

    s_is_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "Testing STA Wi-Fi connection to '%s' while keeping AP open...", ssid);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        return false;
    }
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

const char* wifi_manager_get_ip(void)
{
    return s_ip_str;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t *max_ap_count)
{
    if (!ap_records || !max_ap_count || *max_ap_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 250,
    };

    ESP_LOGI(TAG, "Starting Wi-Fi scan...");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_scan_get_ap_records(max_ap_count, ap_records);
    ESP_LOGI(TAG, "Wi-Fi scan finished, found %u APs", (unsigned)*max_ap_count);
    return ret;
}

int wifi_manager_get_rssi(void)
{
    if (!wifi_manager_is_connected()) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return (int)ap_info.rssi;
    }
    return 0;
}
