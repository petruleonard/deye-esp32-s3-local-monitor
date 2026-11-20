#include "local_web_server.h"
#include "app_config.h"
#include "app_settings.h"
#include "deye_data.h"
#include "deye_modbus.h"
#include "history_store.h"
#include "logger_discovery.h"
#include "solarman_v5.h"
#include "sys_metrics.h"
#include "telegram_notify.h"
#include "wifi_manager.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "local_web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *PORTAL_HTML =
"<!DOCTYPE html><html lang='ro'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Configurare ESP32-S3 Deye Local</title>"
"<style>"
"*{box-sizing:border-box;font-family:-apple-system,Segoe UI,Roboto,sans-serif;}"
"body{margin:0;padding:16px;background:#0b1120;color:#f8fafc;}"
".card{max-width:460px;margin:0 auto;background:#111827;border:1px solid #1f2937;border-radius:16px;padding:18px;}"
"h2{margin:0 0 6px 0;font-size:18px;color:#38bdf8;}"
"p{margin:0 0 14px 0;font-size:12px;color:#94a3b8;}"
"label{display:block;font-size:12px;font-weight:700;color:#cbd5e1;margin:10px 0 4px 0;}"
"input{width:100%;padding:10px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:14px;}"
"button{width:100%;padding:11px;border:none;border-radius:10px;font-weight:700;font-size:14px;cursor:pointer;margin-top:12px;}"
".btn-scan{background:#1e293b;color:#38bdf8;border:1px solid #38bdf8;}"
".btn-save{background:#10b981;color:#052e16;margin-top:18px;}"
".btn-save:disabled{opacity:0.6;cursor:wait;}"
".nets{margin-top:8px;display:flex;flex-direction:column;gap:6px;max-height:180px;overflow-y:auto;}"
".net{padding:8px 10px;background:#0f172a;border:1px solid #334155;border-radius:8px;font-size:13px;display:flex;justify-content:space-between;cursor:pointer;}"
".net:hover{border-color:#38bdf8;}"
"#msg{margin-top:12px;font-size:13px;font-weight:700;text-align:center;line-height:1.4;}"
"</style></head><body><div class='card'>"
"<h2>&#9881; Configurare Wi-Fi ESP32-S3</h2>"
"<p>Alege reteaua Wi-Fi locala. Restul setarilor (scanare invertor, OTA, istoric) se fac apoi din http://deye.local/.</p>"
"<button class='btn-scan' onclick='scanWifi()'>Scaneaza Retele Wi-Fi</button>"
"<div class='nets' id='nets'></div>"
"<label>Retea Wi-Fi (SSID)</label><input id='ssid' placeholder='Selecteaza sau scrie SSID'>"
"<label>Parola Wi-Fi</label><input id='pass' type='password' placeholder='Parola Wi-Fi'>"
"<label>Cod PIN Setari (optional)</label><input id='pin' placeholder='Ex: 1234 (lasa gol pt. fara PIN)'>"
"<button class='btn-save' id='saveBtn' onclick='saveCfg()'>Conecteaza &amp; Salveaza Wi-Fi</button>"
"<div id='msg'></div></div>"
"<script>"
"function scanWifi(){"
"const box=document.getElementById('nets');box.innerHTML='Se scaneaza...';"
"fetch('/scan').then(r=>r.json()).then(arr=>{"
"box.innerHTML='';"
"if(!arr.length){box.innerHTML='<div class=net>Nicio retea gasita</div>';return;}"
"arr.forEach(n=>{"
"const d=document.createElement('div');d.className='net';"
"d.innerHTML='<span>'+n.ssid+'</span><span>'+n.rssi+' dBm</span>';"
"d.onclick=()=>{document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus();};"
"box.appendChild(d);"
"});}).catch(()=>{box.innerHTML='Eroare scanare';});}"
"function saveCfg(){"
"const btn=document.getElementById('saveBtn');"
"const m=document.getElementById('msg');"
"const ssid=document.getElementById('ssid').value.trim();"
"if(!ssid){m.style.color='#ef4444';m.innerText='Introdu numele retelei Wi-Fi!';return;}"
"btn.disabled=true;m.style.color='#38bdf8';"
"m.innerText='Se verifica conexiunea Wi-Fi... (max 12s)';"
"const p={wifi_ssid:ssid,wifi_password:document.getElementById('pass').value,admin_pin:document.getElementById('pin').value.trim()};"
"fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)})"
".then(r=>r.json()).then(d=>{"
"btn.disabled=false;"
"if(d.ok){"
"document.querySelector('.card').innerHTML="
"'<div style=\"text-align:center;padding:8px 0;\">'+"
"'<div style=\"font-size:40px;margin-bottom:8px;\">&#9989;</div>'+"
"'<h2 style=\"color:#10b981;margin-bottom:8px;\">Wi-Fi salvat!</h2>'+"
"'<p style=\"color:#cbd5e1;font-size:13px;margin-bottom:14px;\">Conexiune reusita! Hotspotul se inchide.</p>'+"
"'<div style=\"text-align:left;background:#0f172a;border:1px solid #1e293b;border-radius:12px;padding:12px;font-size:13px;line-height:1.6;color:#e2e8f0;\">'+"
"'Telefonul va reveni pe Wi-Fi-ul casei si va deschide automat aplicatia in ~10 secunde:<br><br>'+"
"'<a href=\"http://deye.local/\" style=\"display:inline-block;padding:10px 16px;background:#0284c7;color:#fff;border-radius:8px;text-decoration:none;font-weight:700;\">Deschide Deye Monitor Local</a>'+"
"'</div></div>';"
"setTimeout(()=>{window.location.href='http://deye.local/';},10000);}"
"else{m.style.color='#ef4444';m.innerText=d.error||'Eroare validare Wi-Fi!';}"
"}).catch(()=>{btn.disabled=false;m.style.color='#ef4444';m.innerText='Eroare comunicatie cu ESP32-S3';});}"
"scanWifi();"
"</script></body></html>";

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    mdns_free();
    wifi_manager_prepare_reboot();
    esp_restart();
}

typedef struct {
    uint32_t ip;
    uint8_t fails;
    int64_t locked_until_us;
} pin_lock_entry_t;

static pin_lock_entry_t s_pin_locks[8] = {0};

static uint32_t get_client_ip_u32(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == 0) {
        if (addr.sin6_family == AF_INET) {
            return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
        }
        return addr.sin6_addr.un.u32_addr[3];
    }
    return 0;
}

static pin_lock_entry_t *find_or_alloc_lock(uint32_t ip) {
    int free_idx = 0;
    for (int i = 0; i < 8; i++) {
        if (s_pin_locks[i].ip == ip) return &s_pin_locks[i];
        if (s_pin_locks[i].ip == 0) free_idx = i;
    }
    s_pin_locks[free_idx].ip = ip;
    s_pin_locks[free_idx].fails = 0;
    s_pin_locks[free_idx].locked_until_us = 0;
    return &s_pin_locks[free_idx];
}

static int get_pin_lock_remaining_sec(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 8; i++) {
        if (s_pin_locks[i].ip == ip && s_pin_locks[i].locked_until_us > now) {
            return (int)((s_pin_locks[i].locked_until_us - now) / 1000000LL) + 1;
        }
    }
    return 0;
}

static bool check_admin_pin_hdr(httpd_req_t *req) {
    if (g_settings.admin_pin[0] == '\0') return true;
    char hdr[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Pin", hdr, sizeof(hdr)) == ESP_OK &&
        strcmp(hdr, g_settings.admin_pin) == 0) {
        return true;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"PIN invalid sau lipsa\"}", HTTPD_RESP_USE_STRLEN);
    return false;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (*p != '\"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < out_sz) {
        if (*p == '\\' && *(p + 1)) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!isdigit((unsigned char)*p) && *p != '-') return false;
    *out = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

static void json_escape(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_sz; i++) {
        char c = src[i];
        if (c == '\"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static esp_err_t read_req_body(httpd_req_t *req, char *buf, size_t buf_sz) {
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char host[64] = {0};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (wifi_manager_is_ap_active() && (!wifi_manager_is_connected() || strncmp(host, "192.168.4.1", 11) == 0)) {
        return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }

    size_t len = (size_t)(index_html_end - index_html_start);
    if (len > 0 && index_html_start[len - 1] == '\0') {
        len--;
    }
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t setup_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_ap_record_t records[16];
    uint16_t count = 16;
    esp_err_t err = wifi_manager_scan(records, &count);

    char *resp = malloc(2048);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(resp + pos, 2048 - pos, "[");
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            char esc_ssid[96];
            json_escape((const char *)records[i].ssid, esc_ssid, sizeof(esc_ssid));
            if (esc_ssid[0] == '\0') continue;
            if (pos > 1) pos += snprintf(resp + pos, 2048 - pos, ",");
            pos += snprintf(resp + pos, 2048 - pos,
                            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                            esc_ssid, records[i].rssi, records[i].authmode);
            if (pos > 1900) break;
        }
    }
    snprintf(resp + pos, 2048 - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char body[768];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    app_settings_t cfg = g_settings;
    json_get_str(body, "wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    json_get_str(body, "wifi_password", cfg.wifi_password, sizeof(cfg.wifi_password));
    if (!json_get_str(body, "admin_pin", cfg.admin_pin, sizeof(cfg.admin_pin))) {
        cfg.admin_pin[0] = '\0';
    }
    json_get_str(body, "logger_mac", cfg.logger_mac, sizeof(cfg.logger_mac));
    json_get_str(body, "logger_ip", cfg.logger_ip, sizeof(cfg.logger_ip));
    json_get_u32(body, "logger_sn", &cfg.logger_sn);
    uint32_t intv = 0;
    if (json_get_u32(body, "send_interval_ms", &intv) && intv >= 5000 && intv <= 60000) {
        cfg.send_interval_ms = intv;
    }

    httpd_resp_set_type(req, "application/json");

    if (cfg.wifi_ssid[0] == '\0') {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"SSID lipsa\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!wifi_manager_test_connect(cfg.wifi_ssid, cfg.wifi_password, 12000)) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut conecta la Wi-Fi (verifica parola)\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cfg.is_configured = true;
    app_settings_save(&cfg);

    httpd_resp_send(req, "{\"ok\":true,\"redirect\":\"http://deye.local/\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t telemetry_get_handler(httpd_req_t *req) {
    deye_data_t d;
    deye_data_get(&d);
    sys_metrics_t sysm;
    sys_metrics_get(&sysm);

    char *buf = malloc(2560);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char esc_src[32];
    char esc_status[96];
    json_escape(d.load_source_str, esc_src, sizeof(esc_src));
    json_escape(d.status_str, esc_status, sizeof(esc_status));

    int lock_sec = get_pin_lock_remaining_sec(get_client_ip_u32(req));
    bool pin_req = (g_settings.admin_pin[0] != '\0');

    snprintf(buf, 2560,
        "{\"online\":%s,\"pin_required\":%s,\"pin_locked_sec\":%d,\"data\":{"
        "\"pv_power_w\":%ld,\"pv1_power_w\":%ld,\"pv2_power_w\":%ld,\"pv3_power_w\":%ld,\"pv4_power_w\":%ld,"
        "\"pv1_volts\":%.1f,\"pv2_volts\":%.1f,\"pv_today_kwh\":%.2f,\"pv_total_kwh\":%.2f,"
        "\"battery_power_w\":%ld,\"battery_soc\":%u,\"battery_soh\":%u,"
        "\"battery_volts\":%.1f,\"battery_current\":%.1f,\"battery_temp_c\":%.1f,\"battery_capacity_kwh\":%.1f,"
        "\"battery_charge_today_kwh\":%.2f,\"battery_discharge_today_kwh\":%.2f,"
        "\"battery_charge_total_kwh\":%.2f,\"battery_discharge_total_kwh\":%.2f,"
        "\"bat_max_chg_a\":%u,\"bat_norm_chg_a\":%u,\"bat_sched_enabled\":%s,"
        "\"bat_pause_start\":%u,\"bat_pause_end\":%u,\"tg_bms_alert\":%s,\"tg_grid_alert\":%s,"
        "\"tg_bat_freeze_alert\":%s,\"bat_freeze_temp\":%d,"
        "\"grid_power_w\":%ld,\"grid_volts\":%.1f,\"grid_l2_volts\":%.1f,\"grid_l3_volts\":%.1f,"
        "\"grid_l1_power_w\":%ld,\"grid_l2_power_w\":%ld,\"grid_l3_power_w\":%ld,\"grid_hz\":%.2f,"
        "\"grid_buy_today_kwh\":%.2f,\"grid_sell_today_kwh\":%.2f,"
        "\"grid_buy_total_kwh\":%.2f,\"grid_sell_total_kwh\":%.2f,"
        "\"grid_outages_7d\":%u,\"grid_outages_14d\":%u,\"grid_outages_30d\":%u,"
        "\"load_power_w\":%ld,\"load_today_kwh\":%.2f,\"load_total_kwh\":%.2f,"
        "\"load_peak_today_kw\":%.2f,\"load_avg_hourly_kw\":%.2f,\"load_pf\":%.2f,"
        "\"load_source_str\":\"%s\",\"inverter_status\":%u,\"inverter_temp_c\":%.1f,"
        "\"inv_phases\":%u,\"inv_dev_type\":%u,"
        "\"cpu_pct\":%d,\"ram_pct\":%d,\"ram_used_kb\":%lu,\"ram_total_kb\":%lu,"
        "\"psram_pct\":%d,\"storage_pct\":%d,\"storage_used_kb\":%lu,\"storage_total_kb\":%lu,"
        "\"spiffs_used_kb\":%lu,\"spiffs_total_kb\":%lu,\"esp_temp_c\":%.1f,\"esp_temp_ok\":%s,"
        "\"wifi_connected\":%s,\"wifi_rssi\":%d,\"logger_connected\":%s,\"status_str\":\"%s\",\"last_update_sec\":%lu"
        "}}",
        d.logger_connected ? "true" : "false",
        pin_req ? "true" : "false",
        lock_sec,
        (long)d.pv_power_w, (long)d.pv1_power_w, (long)d.pv2_power_w, (long)d.pv3_power_w, (long)d.pv4_power_w,
        d.pv1_volts, d.pv2_volts, d.pv_today_kwh, d.pv_total_kwh,
        (long)d.battery_power_w, (unsigned)d.battery_soc, (unsigned)d.battery_soh,
        d.battery_volts, d.battery_current, d.battery_temp_c, d.battery_capacity_kwh,
        d.battery_charge_today_kwh, d.battery_discharge_today_kwh,
        d.battery_charge_total_kwh, d.battery_discharge_total_kwh,
        (unsigned)d.bat_max_chg_a, (unsigned)g_settings.bat_norm_chg_a,
        g_settings.bat_sched_enabled ? "true" : "false",
        (unsigned)g_settings.bat_pause_start_min, (unsigned)g_settings.bat_pause_end_min,
        g_settings.tg_bms_alert_enabled ? "true" : "false",
        g_settings.tg_grid_alert_enabled ? "true" : "false",
        g_settings.tg_bat_freeze_alert_enabled ? "true" : "false",
        (int)g_settings.bat_freeze_temp_c,
        (long)d.grid_power_w, d.grid_volts, d.grid_l2_volts, d.grid_l3_volts,
        (long)d.grid_l1_power_w, (long)d.grid_l2_power_w, (long)d.grid_l3_power_w, d.grid_hz,
        d.grid_buy_today_kwh, d.grid_sell_today_kwh,
        d.grid_buy_total_kwh, d.grid_sell_total_kwh,
        (unsigned)d.grid_outages_7d, (unsigned)d.grid_outages_14d, (unsigned)d.grid_outages_30d,
        (long)d.load_power_w, d.load_today_kwh, d.load_total_kwh,
        d.load_peak_today_kw, d.load_avg_hourly_kw, d.load_pf,
        esc_src, (unsigned)d.inverter_status, d.inverter_temp_c,
        (unsigned)(g_settings.inv_phases ? g_settings.inv_phases : 1), (unsigned)g_settings.inv_dev_type,
        sysm.cpu_pct, sysm.ram_pct, (unsigned long)sysm.ram_used_kb, (unsigned long)sysm.ram_total_kb,
        sysm.psram_pct, sysm.storage_pct, (unsigned long)sysm.storage_used_kb, (unsigned long)sysm.storage_total_kb,
        (unsigned long)sysm.spiffs_used_kb, (unsigned long)sysm.spiffs_total_kb,
        sysm.esp_temp_c, sysm.esp_temp_ok ? "true" : "false",
        d.wifi_connected ? "true" : "false", wifi_manager_get_rssi(), d.logger_connected ? "true" : "false",
        esc_status, (unsigned long)d.last_update_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t history_get_handler(httpd_req_t *req) {
    char query[128] = {0};
    char period[16] = "day";
    char date_str[16] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "period", period, sizeof(period));
        httpd_query_key_value(query, "date", date_str, sizeof(date_str));
    }

    size_t buf_sz = 32768;
    char *buf = malloc(buf_sz);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    history_store_query(period, date_str, buf, buf_sz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t pin_verify_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (g_settings.admin_pin[0] == '\0') {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint32_t ip = get_client_ip_u32(req);
    int rem = get_pin_lock_remaining_sec(ip);
    if (rem > 0) {
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"locked\":true,\"remaining_sec\":%d,\"error\":\"Acces blocat! Mai asteapta %d min.\"}",
                 rem, (rem / 60) + 1);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char body[128];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    char pin_in[24] = {0};
    json_get_str(body, "pin", pin_in, sizeof(pin_in));
    pin_lock_entry_t *entry = find_or_alloc_lock(ip);

    if (strcmp(pin_in, g_settings.admin_pin) == 0) {
        entry->fails = 0;
        entry->locked_until_us = 0;
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    entry->fails++;
    if (entry->fails >= 3) {
        entry->fails = 0;
        entry->locked_until_us = esp_timer_get_time() + 3600LL * 1000000LL;
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req,
            "{\"ok\":false,\"locked\":true,\"remaining_sec\":3600,\"error\":\"Ai gresit PIN-ul de 3 ori! Acces blocat 1 ora.\"}",
            HTTPD_RESP_USE_STRLEN);
    } else {
        int left = 3 - entry->fails;
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"locked\":false,\"attempts_left\":%d,\"error\":\"PIN gresit! Mai ai %d incercari.\"}",
                 left, left);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t history_clear_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    bool ok = history_store_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Eroare stergere\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char buf[448];
    char esc_mac[32], esc_ip[32], esc_tok[128], esc_chat[48];
    json_escape(g_settings.logger_mac, esc_mac, sizeof(esc_mac));
    json_escape(g_settings.logger_ip, esc_ip, sizeof(esc_ip));
    json_escape(g_settings.tg_bot_token, esc_tok, sizeof(esc_tok));
    json_escape(g_settings.tg_chat_id, esc_chat, sizeof(esc_chat));

    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *part_lbl = running ? running->label : "ota_0";

    snprintf(buf, sizeof(buf),
             "{\"logger_mac\":\"%s\",\"logger_ip\":\"%s\",\"logger_sn\":%lu,"
             "\"inv_phases\":%u,\"inv_dev_type\":%u,"
             "\"send_interval_ms\":%lu,\"tg_bot_token\":\"%s\",\"tg_chat_id\":\"%s\","
             "\"fw_version\":\"%s (%s)\"}",
             esc_mac, esc_ip, (unsigned long)g_settings.logger_sn,
             (unsigned)g_settings.inv_phases, (unsigned)g_settings.inv_dev_type,
             (unsigned long)g_settings.send_interval_ms, esc_tok, esc_chat,
             FW_VERSION, part_lbl);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char body[640];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    app_settings_t cfg = g_settings;
    json_get_str(body, "logger_mac", cfg.logger_mac, sizeof(cfg.logger_mac));
    json_get_str(body, "logger_ip", cfg.logger_ip, sizeof(cfg.logger_ip));
    json_get_u32(body, "logger_sn", &cfg.logger_sn);
    json_get_str(body, "tg_bot_token", cfg.tg_bot_token, sizeof(cfg.tg_bot_token));
    json_get_str(body, "tg_chat_id", cfg.tg_chat_id, sizeof(cfg.tg_chat_id));
    uint32_t intv = 0;
    if (json_get_u32(body, "send_interval_ms", &intv) && intv >= 5000 && intv <= 60000) {
        cfg.send_interval_ms = intv;
    }

    app_settings_save(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool json_get_bool(const char *json, const char *key, bool *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static esp_err_t battery_config_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char body[256];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }
    app_settings_t cfg = g_settings;
    bool bval = false;
    if (json_get_bool(body, "bat_sched_enabled", &bval)) cfg.bat_sched_enabled = bval;
    if (json_get_bool(body, "tg_bms_alert", &bval)) cfg.tg_bms_alert_enabled = bval;
    if (json_get_bool(body, "tg_grid_alert", &bval)) cfg.tg_grid_alert_enabled = bval;
    if (json_get_bool(body, "tg_bat_freeze_alert", &bval)) cfg.tg_bat_freeze_alert_enabled = bval;
    const char *pfrz = strstr(body, "\"bat_freeze_temp\"");
    if (pfrz) {
        const char *c = strchr(pfrz, ':');
        if (c) {
            int tv = (int)strtol(c + 1, NULL, 10);
            if (tv >= -10 && tv <= 25) cfg.bat_freeze_temp_c = (int8_t)tv;
        }
    }
    uint32_t uval = 0;
    if (json_get_u32(body, "bat_pause_start", &uval) && uval < 1440) cfg.bat_pause_start_min = (uint16_t)uval;
    if (json_get_u32(body, "bat_pause_end", &uval) && uval < 1440) cfg.bat_pause_end_min = (uint16_t)uval;
    app_settings_save(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t telegram_test_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    esp_err_t err = telegram_notify_send_test();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Completeaza mai intai Bot Token si Chat ID\"}",
                        HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut trimite (verifica Token/Chat ID/internet)\"}",
                        HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static volatile bool s_logger_scanning = false;
static volatile bool s_logger_scan_done = false;
static discovered_logger_t s_scan_results[4];
static int s_scan_count = 0;

static void logger_scan_bg_task(void *arg) {
    deye_modbus_set_scan_paused(true);

    discovered_logger_t tmp[4];
    int n = logger_discover_all(tmp, 4);
    if (n > 0) {
        memcpy(s_scan_results, tmp, sizeof(discovered_logger_t) * (size_t)n);
    }
    s_scan_count = n;
    s_logger_scan_done = true;
    s_logger_scanning = false;

    deye_modbus_set_scan_paused(false);
    vTaskDelete(NULL);
}

static esp_err_t logger_scan_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    if (!s_logger_scanning) {
        s_logger_scan_done = false;
        s_scan_count = 0;
        s_logger_scanning = true;
        if (xTaskCreate(logger_scan_bg_task, "logger_scan", 5120, NULL, 4, NULL) != pdPASS) {
            s_logger_scanning = false;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"scanning\":true,\"done\":false,\"loggers\":[]}");
    return ESP_OK;
}

static esp_err_t logger_scan_status_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    char *buf = malloc(1024);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    bool scanning = s_logger_scanning;
    bool done = s_logger_scan_done;
    int n = done ? s_scan_count : 0;
    size_t pos = snprintf(buf, 1024, "{\"scanning\":%s,\"done\":%s,\"loggers\":[",
                          scanning ? "true" : "false", done ? "true" : "false");
    for (int i = 0; i < n; i++) {
        char esc_model[48];
        json_escape(s_scan_results[i].model, esc_model, sizeof(esc_model));
        pos += snprintf(buf + pos, 1024 - pos,
                        "%s{\"ip\":\"%s\",\"mac\":\"%s\",\"sn\":%lu,\"model\":\"%s\"}",
                        (i > 0) ? "," : "",
                        s_scan_results[i].ip, s_scan_results[i].mac,
                        (unsigned long)s_scan_results[i].sn, esc_model);
        if (pos > 896) break;
    }
    snprintf(buf + pos, 1024 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t chart_get_handler(httpd_req_t *req) {
    char *buf = malloc(8192);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    history_store_chart_json(buf, 8192);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t chart_fasttest_post_handler(httpd_req_t *req) {
    history_store_enable_fast_test(true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    if (req->content_len <= 0 || req->content_len > 0x300000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Dimensiune fisier .bin invalida\"}");
        return ESP_FAIL;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Partitie OTA indisponibila\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut porni OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = (remaining > 2048) ? 2048 : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare receptie fisier\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare scriere Flash\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Imagine .bin invalida\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare activare partitie OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA reusit pe partitia '%s'! Repornesc...", update_part->label);
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

typedef struct {
    char state[16];
    int progress;
    char message[128];
    char bin_url[256];
} github_ota_status_t;

static github_ota_status_t s_gh_ota = {
    .state = "idle",
    .progress = 0,
    .message = "",
    .bin_url = GITHUB_OTA_BIN_URL
};

static bool extract_json_str(const char *json, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

bool github_ota_check_latest(char *out_ver, size_t ver_sz, char *out_url, size_t url_sz, char *out_notes, size_t notes_sz) {
    esp_http_client_config_t cfg = {
        .url = GITHUB_OTA_VERSION_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len > 2048) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = esp_http_client_read(client, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!extract_json_str(buf, "version", out_ver, ver_sz)) return false;
    if (!extract_json_str(buf, "url", out_url, url_sz) || strncmp(out_url, "https://", 8) != 0) {
        snprintf(out_url, url_sz, "%s", GITHUB_OTA_BIN_URL);
    }
    extract_json_str(buf, "notes", out_notes, notes_sz);
    return out_ver[0] != '\0';
}

static esp_err_t ota_check_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");

    char latest[32] = "";
    char url[256] = GITHUB_OTA_BIN_URL;
    char notes[160] = "";
    if (!github_ota_check_latest(latest, sizeof(latest), url, sizeof(url), notes, sizeof(notes))) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut contacta GitHub (verifica conexiunea la internet)\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    snprintf(s_gh_ota.bin_url, sizeof(s_gh_ota.bin_url), "%s", url);
    bool avail = (strcmp(latest, FW_VERSION) != 0);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"current\":\"%s\",\"latest\":\"%s\",\"update_available\":%s,\"notes\":\"%s\"}",
             FW_VERSION, latest, avail ? "true" : "false", notes);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ota_github_task(void *arg) {
    (void)arg;
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "running");
    s_gh_ota.progress = 0;
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Conectare la GitHub...");

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Partitie OTA indisponibila");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = s_gh_ota.bin_url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare initializare HTTP client");
        vTaskDelete(NULL);
        return;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Nu s-a putut deschide conexiunea HTTPS");
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len <= 0 || content_len > 0x300000) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Fisier firmware indisponibil (HTTP %d)", status);
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare pornire scriere Flash");
        vTaskDelete(NULL);
        return;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Memorie insuficienta");
        vTaskDelete(NULL);
        return;
    }

    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Se descarca firmware de pe GitHub...");
    int written = 0;
    while (written < content_len) {
        int r = esp_http_client_read(client, buf, 4096);
        if (r < 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
            snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Conexiune intrerupta in timpul descarcarii");
            vTaskDelete(NULL);
            return;
        }
        if (r == 0) break;
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
            snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare scriere Flash");
            vTaskDelete(NULL);
            return;
        }
        written += r;
        s_gh_ota.progress = (written * 100) / content_len;
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (written != content_len || esp_ota_end(ota_handle) != ESP_OK) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Imagine .bin incompleta sau invalida");
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare activare partitie OTA");
        vTaskDelete(NULL);
        return;
    }

    s_gh_ota.progress = 100;
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "done");
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Actualizare reusita! ESP32-S3 reporneste...");
    ESP_LOGI(TAG, "GitHub OTA reusit pe '%s'! Repornesc in 1.0s...", update_part->label);
    vTaskDelay(pdMS_TO_TICKS(1000));
    mdns_free();
    wifi_manager_prepare_reboot();
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t ota_github_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    if (strcmp(s_gh_ota.state, "running") == 0) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Actualizarea ruleaza deja\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "running");
    s_gh_ota.progress = 0;
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Pornire actualizare GitHub...");
    xTaskCreate(ota_github_task, "ota_github", 8192, NULL, 5, NULL);
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_status_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
             s_gh_ota.state, s_gh_ota.progress, s_gh_ota.message);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    if (wifi_manager_is_ap_active()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

static void captive_dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[256];
    while (1) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&client, &clen);
        if (len < 12 || !wifi_manager_is_ap_active()) continue;

        int q = 12;
        while (q < len && buf[q] != 0) q += buf[q] + 1;
        q += 5;
        if (q > len || q + 16 > (int)sizeof(buf)) continue;

        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;
        int idx = q;
        buf[idx++] = 0xC0; buf[idx++] = 0x0C;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 1;
        buf[idx++] = 0x00; buf[idx++] = 0x04;
        buf[idx++] = 192;  buf[idx++] = 168;  buf[idx++] = 4;    buf[idx++] = 1;
        sendto(sock, buf, idx, 0, (struct sockaddr *)&client, clen);
    }
}

esp_err_t local_web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 28;
    config.max_open_sockets = 8;
    config.backlog_conn = 8;
    config.lru_purge_enable = true;
    config.stack_size = 10240;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Nu s-a putut porni serverul HTTP: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                      .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/setup",                 .method = HTTP_GET,  .handler = setup_get_handler },
        { .uri = "/scan",                  .method = HTTP_GET,  .handler = scan_get_handler },
        { .uri = "/save",                  .method = HTTP_POST, .handler = save_post_handler },
        { .uri = "/api/telemetry",         .method = HTTP_GET,  .handler = telemetry_get_handler },
        { .uri = "/api/history",           .method = HTTP_GET,  .handler = history_get_handler },
        { .uri = "/api/chart",             .method = HTTP_GET,  .handler = chart_get_handler },
        { .uri = "/api/chart/fasttest",    .method = HTTP_POST, .handler = chart_fasttest_post_handler },
        { .uri = "/api/history/clear",     .method = HTTP_POST, .handler = history_clear_post_handler },
        { .uri = "/api/config",            .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",            .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/battery/config",    .method = HTTP_POST, .handler = battery_config_post_handler },
        { .uri = "/api/telegram/test",     .method = HTTP_POST, .handler = telegram_test_post_handler },
        { .uri = "/api/logger/scan",       .method = HTTP_GET,  .handler = logger_scan_get_handler },
        { .uri = "/api/logger/scan/status",.method = HTTP_GET,  .handler = logger_scan_status_get_handler },
        { .uri = "/api/ota",               .method = HTTP_POST, .handler = ota_post_handler },
        { .uri = "/api/ota/check",         .method = HTTP_GET,  .handler = ota_check_get_handler },
        { .uri = "/api/ota/github",        .method = HTTP_POST, .handler = ota_github_post_handler },
        { .uri = "/api/ota/status",        .method = HTTP_GET,  .handler = ota_status_get_handler },
        { .uri = "/api/pin/verify",        .method = HTTP_POST, .handler = pin_verify_post_handler },
        { .uri = "/*",                     .method = HTTP_GET,  .handler = captive_redirect_handler },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Server web local + Captive Portal pornit (http://deye.local/)");
    return ESP_OK;
}
