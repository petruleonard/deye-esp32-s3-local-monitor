#include "logger_discovery.h"
#include "solarman_v5.h"
#include "app_settings.h"
#include "deye_data.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "logger_disc";
#define SOLARMAN_DISC_PORT 48899
#define SOLARMAN_TCP_PORT  8899

static const char *DISC_CMDS[] = {
    "WIFIKIT-214028-READ",
    "HF-A11ASSISTHREAD",
};

static void normalize_mac(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    if (!src || dst_sz == 0) {
        if (dst_sz > 0) dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c)) {
            dst[j++] = (char)toupper(c);
        }
    }
    dst[j] = '\0';
}

static bool parse_disc_reply(char *rx_buf, const struct sockaddr_in *src_addr,
                             char *out_ip, size_t out_ip_sz,
                             char *out_mac, size_t out_mac_sz, uint32_t *out_sn)
{
    char *p_ip = rx_buf;
    char *c1 = strchr(p_ip, ',');
    if (!c1) return false;
    *c1 = '\0';

    char *p_mac = c1 + 1;
    char *c2 = strchr(p_mac, ',');
    if (!c2) return false;
    *c2 = '\0';

    char *p_sn = c2 + 1;
    for (int k = (int)strlen(p_sn) - 1; k >= 0; k--) {
        if (p_sn[k] == '\r' || p_sn[k] == '\n' || p_sn[k] == ' ') p_sn[k] = '\0';
        else break;
    }

    if (strlen(p_ip) < 7 || strcmp(p_ip, "0.0.0.0") == 0) {
        ip4addr_ntoa_r((const ip4_addr_t *)&src_addr->sin_addr, out_ip, (int)out_ip_sz);
    } else {
        strncpy(out_ip, p_ip, out_ip_sz - 1);
        out_ip[out_ip_sz - 1] = '\0';
    }

    normalize_mac(p_mac, out_mac, out_mac_sz);
    *out_sn = (uint32_t)strtoul(p_sn, NULL, 10);
    return (*out_sn > 0 || strlen(out_mac) == 12);
}

static void add_discovered_unique(discovered_logger_t *list, int *count, int max_count,
                                  const char *ip, const char *mac, uint32_t sn)
{
    if (*count >= max_count) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(list[i].ip, ip) == 0 && (sn == 0 || list[i].sn == sn)) {
            return;
        }
        if (mac[0] != '\0' && strcmp(list[i].mac, mac) == 0 && list[i].sn == sn) {
            return;
        }
    }
    memset(&list[*count], 0, sizeof(discovered_logger_t));
    strncpy(list[*count].ip, ip, sizeof(list[*count].ip) - 1);
    strncpy(list[*count].mac, mac, sizeof(list[*count].mac) - 1);
    list[*count].sn = sn;
    (*count)++;
}

static void udp_broadcast_discover(const esp_netif_ip_info_t *sta_ip,
                                   discovered_logger_t *list, int *count, int max_count)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    int broadcast = 1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 600000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(0);
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    struct sockaddr_in bcast_sub = {0};
    bcast_sub.sin_family = AF_INET;
    bcast_sub.sin_port = htons(SOLARMAN_DISC_PORT);
    bool has_sub_bcast = (sta_ip && sta_ip->ip.addr != 0 && sta_ip->netmask.addr != 0);
    if (has_sub_bcast) {
        bcast_sub.sin_addr.s_addr = (sta_ip->ip.addr & sta_ip->netmask.addr) | (~sta_ip->netmask.addr);
    }

    struct sockaddr_in bcast_global = {0};
    bcast_global.sin_family = AF_INET;
    bcast_global.sin_port = htons(SOLARMAN_DISC_PORT);
    bcast_global.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (int round = 0; round < 3; round++) {
        const char *cmd = (round < 2) ? DISC_CMDS[0] : DISC_CMDS[1];
        size_t cmd_len = strlen(cmd);
        if (has_sub_bcast) {
            sendto(sock, cmd, cmd_len, 0, (struct sockaddr *)&bcast_sub, sizeof(bcast_sub));
        }
        sendto(sock, cmd, cmd_len, 0, (struct sockaddr *)&bcast_global, sizeof(bcast_global));

        char rx_buf[160];
        while (*count < max_count) {
            struct sockaddr_in src_addr = {0};
            socklen_t addr_len = sizeof(src_addr);
            int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                               (struct sockaddr *)&src_addr, &addr_len);
            if (len <= 0) break;
            rx_buf[len] = '\0';

            char ip[16] = {0}, mac[20] = {0};
            uint32_t sn = 0;
            if (parse_disc_reply(rx_buf, &src_addr, ip, sizeof(ip), mac, sizeof(mac), &sn)) {
                ESP_LOGI(TAG, "UDP 48899 discovered: IP=%s, MAC=%s, SN=%lu", ip, mac, (unsigned long)sn);
                add_discovered_unique(list, count, max_count, ip, mac, sn);
            }
        }
        if (*count > 0 && round >= 1) break;
    }

    close(sock);
}

static bool udp_unicast_query(const char *target_ip, char *out_mac, size_t out_mac_sz, uint32_t *out_sn)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 450000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SOLARMAN_DISC_PORT);
    if (inet_pton(AF_INET, target_ip, &dst.sin_addr) != 1) {
        close(sock);
        return false;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        const char *cmd = (attempt < 2) ? DISC_CMDS[0] : DISC_CMDS[1];
        sendto(sock, cmd, strlen(cmd), 0, (struct sockaddr *)&dst, sizeof(dst));

        char rx_buf[160];
        struct sockaddr_in src_addr = {0};
        socklen_t addr_len = sizeof(src_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&src_addr, &addr_len);
        if (len > 0) {
            rx_buf[len] = '\0';
            char ip[16] = {0};
            if (parse_disc_reply(rx_buf, &src_addr, ip, sizeof(ip), out_mac, out_mac_sz, out_sn)) {
                close(sock);
                return true;
            }
        }
    }
    close(sock);
    return false;
}

static void tcp8899_scan_prefix(uint32_t base, uint32_t net_ip,
                                discovered_logger_t *list, int *count, int max_count)
{
    const int BATCH = 8;
    int socks[8];
    uint32_t ips[8];

    uint32_t h = 1;
    while (h < 255 && *count < max_count) {
        int n = 0;
        int max_fd = -1;
        fd_set wfds;
        FD_ZERO(&wfds);

        while (n < BATCH && h < 255) {
            uint32_t ip_h = base | h;
            if (ip_h == net_ip || (ip_h & 0xFF) == 0 || (ip_h & 0xFF) == 255) {
                h++;
                continue;
            }

            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s < 0) break;
            h++;

            int flags = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in dst = {0};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(SOLARMAN_TCP_PORT);
            dst.sin_addr.s_addr = htonl(ip_h);

            int rc = connect(s, (struct sockaddr *)&dst, sizeof(dst));
            if (rc < 0 && errno != EINPROGRESS) {
                close(s);
                continue;
            }
            socks[n] = s;
            ips[n] = ip_h;
            FD_SET(s, &wfds);
            if (s > max_fd) max_fd = s;
            n++;
        }

        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 110000 };
        int ready = select(max_fd + 1, NULL, &wfds, NULL, &tv);
        for (int i = 0; i < n; i++) {
            bool open = false;
            if (ready > 0 && FD_ISSET(socks[i], &wfds)) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) {
                    open = true;
                }
            }
            close(socks[i]);

            if (open) {
                struct in_addr ina = { .s_addr = htonl(ips[i]) };
                char cand_ip[16] = {0};
                ip4addr_ntoa_r((const ip4_addr_t *)&ina, cand_ip, sizeof(cand_ip));
                ESP_LOGI(TAG, "TCP 8899 open on %s -> querying UDP 48899 unicast...", cand_ip);

                char mac[20] = {0};
                uint32_t sn = 0;
                if (udp_unicast_query(cand_ip, mac, sizeof(mac), &sn)) {
                    ESP_LOGI(TAG, "Unicast 48899 reply: IP=%s, MAC=%s, SN=%lu", cand_ip, mac, (unsigned long)sn);
                } else {
                    if (g_settings.logger_sn > 0) sn = g_settings.logger_sn;
                    if (g_settings.logger_mac[0] != '\0') {
                        strncpy(mac, g_settings.logger_mac, sizeof(mac) - 1);
                        mac[sizeof(mac) - 1] = '\0';
                    }
                }
                add_discovered_unique(list, count, max_count, cand_ip, mac, sn);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void tcp8899_subnet_scan(const esp_netif_ip_info_t *sta_ip,
                                discovered_logger_t *list, int *count, int max_count)
{
    if (!sta_ip || sta_ip->ip.addr == 0) return;

    uint32_t net_ip = ntohl(sta_ip->ip.addr);
    uint32_t net_mask = ntohl(sta_ip->netmask.addr);
    uint32_t base_primary = (net_ip & net_mask) & 0xFFFFFF00UL;
    uint32_t base_own24   = net_ip & 0xFFFFFF00UL;

    ESP_LOGI(TAG, "UDP 48899 silent -> running TCP %d scan...", SOLARMAN_TCP_PORT);
    tcp8899_scan_prefix(base_primary, net_ip, list, count, max_count);
    if (*count == 0 && base_own24 != base_primary) {
        tcp8899_scan_prefix(base_own24, net_ip, list, count, max_count);
    }
}

int logger_discover_all(discovered_logger_t *out_list, int max_count)
{
    if (!out_list || max_count <= 0) return 0;

    esp_netif_ip_info_t sta_ip = {0};
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_get_ip_info(sta_netif, &sta_ip);
    }

    int count = 0;

    // 1. Check currently configured/connected logger first
    if (strlen(g_settings.logger_ip) >= 7) {
        char mac[20] = {0};
        uint32_t sn = 0;
        deye_data_t cur_d;
        deye_data_get(&cur_d);
        if (udp_unicast_query(g_settings.logger_ip, mac, sizeof(mac), &sn)) {
            add_discovered_unique(out_list, &count, max_count, g_settings.logger_ip, mac, sn);
        } else if (cur_d.logger_connected && g_settings.logger_sn > 0) {
            add_discovered_unique(out_list, &count, max_count,
                                  g_settings.logger_ip, g_settings.logger_mac, g_settings.logger_sn);
        }
    }

    // 2. UDP 48899 broadcast discovery
    udp_broadcast_discover(&sta_ip, out_list, &count, max_count);

    // 3. TCP 8899 subnet scan fallback if none found yet
    if (count == 0) {
        tcp8899_subnet_scan(&sta_ip, out_list, &count, max_count);
    }

    deye_data_t cur_d;
    deye_data_get(&cur_d);
    for (int i = 0; i < count; i++) {
        uint16_t dev_type = 0;
        if (out_list[i].sn > 0 &&
            solarman_v5_read_registers(out_list[i].ip, out_list[i].sn, 0, 1, &dev_type) == ESP_OK) {
            if (dev_type == 0x0500 || dev_type == 0x0600 || dev_type == 0x0601) {
                snprintf(out_list[i].model, sizeof(out_list[i].model), "Deye Trifazat (0x%04X)", dev_type);
            } else {
                snprintf(out_list[i].model, sizeof(out_list[i].model), "Deye Monofazat (0x%04X)", dev_type);
            }
        } else if (cur_d.logger_connected && strcmp(out_list[i].ip, g_settings.logger_ip) == 0) {
            snprintf(out_list[i].model, sizeof(out_list[i].model), "Deye Invertor (Conectat)");
        } else {
            snprintf(out_list[i].model, sizeof(out_list[i].model), "Logger Solarman");
        }
    }

    return count;
}

bool logger_discover_by_mac(const char *target_mac, uint32_t target_sn,
                            char *out_ip, size_t out_ip_sz, uint32_t *out_sn)
{
    char norm_target[20];
    normalize_mac(target_mac, norm_target, sizeof(norm_target));

    discovered_logger_t found[6];
    int n = logger_discover_all(found, 6);
    if (n <= 0) return false;

    for (int i = 0; i < n; i++) {
        bool mac_match = (norm_target[0] != '\0' && strcmp(norm_target, found[i].mac) == 0);
        bool sn_match = (target_sn > 0 && found[i].sn == target_sn);
        if (mac_match || sn_match) {
            strncpy(out_ip, found[i].ip, out_ip_sz - 1);
            out_ip[out_ip_sz - 1] = '\0';
            if (out_sn && found[i].sn > 0) *out_sn = found[i].sn;
            return true;
        }
    }

    for (int i = 0; i < n; i++) {
        if (found[i].sn > 0) {
            strncpy(out_ip, found[i].ip, out_ip_sz - 1);
            out_ip[out_ip_sz - 1] = '\0';
            if (out_sn) *out_sn = found[i].sn;
            ESP_LOGW(TAG, "Zero-config lock onto Solarman logger: IP=%s MAC=%s SN=%lu",
                     found[i].ip, found[i].mac, (unsigned long)found[i].sn);
            return true;
        }
    }

    if (target_sn > 0) {
        for (int i = 0; i < n; i++) {
            uint16_t dev_type = 0;
            if (solarman_v5_read_registers(found[i].ip, target_sn, 0, 1, &dev_type) == ESP_OK) {
                strncpy(out_ip, found[i].ip, out_ip_sz - 1);
                out_ip[out_ip_sz - 1] = '\0';
                if (out_sn) *out_sn = target_sn;
                return true;
            }
        }
    }

    return false;
}
