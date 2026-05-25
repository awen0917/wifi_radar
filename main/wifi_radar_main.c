/**
 * WiFi Radar - ESP32 室内人员感知系统
 * 
 * 功能：
 * 1. WiFi CSI 采集与人员检测
 * 2. 内置 Web 仪表盘
 * 3. 串口波形数据输出
 * 4. WiFi 图形化配置页面
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "lwip/ip4_addr.h"

// ========== 常量定义 ==========
#define TAG "WiFiRadar"

#define NVS_NAMESPACE      "wifi_radar"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASSWORD   "password"
#define NVS_KEY_CONFIGURED "configured"

#define MAX_SSID_LEN       32
#define MAX_PASSWORD_LEN   64

#define CSI_QUEUE_LEN      64
#define CSI_SUBCARRIERS    56   // ESP32 20MHz HT20 子载波数

#define WEB_SERVER_STACK   8192

// 人员检测阈值
#define PRESENCE_THRESHOLD  8.0f
#define WINDOW_SIZE         30

// 串口波形输出端口 (使用默认 USB/UART0)
#define WAVE_UART_NUM       0

// ========== 全局变量 ==========
typedef struct {
    bool is_configured;
    char ssid[MAX_SSID_LEN + 1];
    char password[MAX_PASSWORD_LEN + 1];
} wifi_config_t_custom;

static wifi_config_t_custom g_wifi_cfg = {0};
static bool g_wifi_connected = false;
static bool g_ap_active = false;
static char g_ip_str[16] = "0.0.0.0";
static httpd_handle_t g_server = NULL;
static volatile uint32_t g_csi_count = 0;  // CSI 回调计数

// CSI 数据队列
typedef struct {
    int8_t data[CSI_SUBCARRIERS];  // CSI 幅度数据
    int64_t timestamp;
} csi_sample_t;

static QueueHandle_t g_csi_queue = NULL;
static csi_sample_t g_last_csi = {0};  // 最新 CSI 快照供 SSE 使用

// 人员检测状态
typedef struct {
    bool presence;           // 当前是否有人
    float variance;          // 当前方差
    float rssi;              // 信号强度
    int confidence;          // 置信度 0-100
    int64_t last_change_ts;  // 状态变化时间
} presence_state_t;

static presence_state_t g_presence = {0};
static SemaphoreHandle_t g_presence_mutex = NULL;

// CSI 采集缓冲
static float g_variance_window[WINDOW_SIZE];
static int g_variance_idx = 0;
static bool g_window_filled = false;

// ========== NVS 配置存取 ==========
static esp_err_t load_wifi_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        g_wifi_cfg.is_configured = false;
        return err;
    }

    size_t ssid_len = sizeof(g_wifi_cfg.ssid);
    size_t pass_len = sizeof(g_wifi_cfg.password);

    err = nvs_get_str(handle, NVS_KEY_SSID, g_wifi_cfg.ssid, &ssid_len);
    if (err != ESP_OK) {
        g_wifi_cfg.is_configured = false;
        nvs_close(handle);
        return err;
    }

    err = nvs_get_str(handle, NVS_KEY_PASSWORD, g_wifi_cfg.password, &pass_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        g_wifi_cfg.password[0] = '\0';
    }

    uint8_t configured = 0;
    nvs_get_u8(handle, NVS_KEY_CONFIGURED, &configured);
    g_wifi_cfg.is_configured = (configured == 1);

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded WiFi config: SSID=%s, configured=%d", g_wifi_cfg.ssid, g_wifi_cfg.is_configured);
    return ESP_OK;
}

static esp_err_t save_wifi_config(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_str(handle, NVS_KEY_SSID, ssid);
    nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    nvs_set_u8(handle, NVS_KEY_CONFIGURED, 1);
    nvs_commit(handle);
    nvs_close(handle);

    strncpy(g_wifi_cfg.ssid, ssid, MAX_SSID_LEN);
    strncpy(g_wifi_cfg.password, password, MAX_PASSWORD_LEN);
    g_wifi_cfg.is_configured = true;

    ESP_LOGI(TAG, "Saved WiFi config: SSID=%s", ssid);
    return ESP_OK;
}

// ========== WiFi 事件处理 ==========
static int g_sta_retry_count = 0;
#define STA_MAX_RETRY 5

static void sta_reconnect_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "STA reconnect timer fired, calling esp_wifi_connect()");
    esp_wifi_connect();
}

static TimerHandle_t g_sta_reconnect_timer = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        g_sta_retry_count = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        strcpy(g_ip_str, "0.0.0.0");
        g_sta_retry_count++;
        if (g_sta_retry_count <= STA_MAX_RETRY) {
            int delay_sec = g_sta_retry_count * 2;  // 2s, 4s, 6s, 8s, 10s
            ESP_LOGI(TAG, "WiFi disconnected, retry %d/%d in %ds...", g_sta_retry_count, STA_MAX_RETRY, delay_sec);
            if (!g_sta_reconnect_timer) {
                g_sta_reconnect_timer = xTimerCreate("sta_rcon", pdMS_TO_TICKS(delay_sec * 1000),
                    pdFALSE, NULL, sta_reconnect_timer_cb);
            } else {
                xTimerChangePeriod(g_sta_reconnect_timer, pdMS_TO_TICKS(delay_sec * 1000), 0);
                // xTimerChangePeriod already starts the timer, no need for xTimerStart
            }
        } else {
            ESP_LOGW(TAG, "WiFi connection failed after %d retries, AP mode still active", STA_MAX_RETRY);
            // 不重置 retry_count，停止重连直到用户重新配置
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(g_ip_str, sizeof(g_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", g_ip_str);
        g_wifi_connected = true;
        g_sta_retry_count = 0;
        esp_wifi_set_csi(true);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP: client connected");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "AP: client disconnected");
    }
}

// ========== WiFi AP 模式 (配置用) ==========
static esp_netif_t *g_ap_netif = NULL;
static esp_netif_t *g_sta_netif = NULL;

static void wifi_init_ap(void)
{
    // AP 和 STA 的 netif 必须在 esp_wifi_start() 之前全部创建
    if (!g_ap_netif) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!g_sta_netif) {
        g_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "WiFiRadar_Setup",
            .ssid_len = 0,
            .channel = 1,
            .password = "12345678",
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    g_ap_active = true;
    ESP_LOGI(TAG, "AP mode started: SSID=WiFiRadar_Setup, Password=12345678");
}

// ========== WiFi STA 模式 ==========
static bool g_sta_netif_created = false;

static void wifi_init_sta(const char *ssid, const char *password)
{
    // STA netif 已在 wifi_init_ap() 中提前创建，无需再创建
    // 如果 wifi_init_ap() 尚未调用（不应发生），在此创建
    if (!g_sta_netif) {
        g_sta_netif = esp_netif_create_default_wifi_sta();
    }

    // 停止重连定时器
    if (g_sta_reconnect_timer) {
        xTimerStop(g_sta_reconnect_timer, 0);
    }

    // 先断开当前 STA 连接（忽略错误，可能未连接）
    esp_err_t disc_ret = esp_wifi_disconnect();
    if (disc_ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi_disconnect: %s (ok if not connected)", esp_err_to_name(disc_ret));
    }

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    g_sta_retry_count = 0;
    esp_wifi_connect();

    ESP_LOGI(TAG, "STA connecting to: %s", ssid);
}

// ========== CSI 回调 ==========
static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) return;

    g_csi_count++;

    csi_sample_t sample = {0};
    sample.timestamp = esp_timer_get_time();

    // 提取 CSI 子载波幅度 (I/Q 交替存储)
    int num_subcarriers = info->len / 2;
    if (num_subcarriers > CSI_SUBCARRIERS) num_subcarriers = CSI_SUBCARRIERS;

    for (int i = 0; i < num_subcarriers; i++) {
        int16_t real = (int16_t)info->buf[i * 2];
        int16_t imag = (int16_t)info->buf[i * 2 + 1];
        sample.data[i] = (int8_t)sqrtf((float)(real * real + imag * imag));
    }

    // 保存最新快照供 API 使用
    g_last_csi = sample;

    // 存入队列（非阻塞，满则丢弃最旧）
    if (xQueueSend(g_csi_queue, &sample, 0) != pdTRUE) {
        csi_sample_t dummy;
        xQueueReceive(g_csi_queue, &dummy, 0);
        xQueueSend(g_csi_queue, &sample, 0);
    }

    // 更新 RSSI
    if (xSemaphoreTake(g_presence_mutex, 0) == pdTRUE) {
        g_presence.rssi = info->rx_ctrl.rssi;
        xSemaphoreGive(g_presence_mutex);
    }
}

// ========== 人员检测算法 ==========
static float compute_variance(int8_t *data, int len)
{
    if (len <= 0) return 0;

    float sum = 0, sum_sq = 0;
    for (int i = 0; i < len; i++) {
        sum += (float)data[i];
        sum_sq += (float)data[i] * data[i];
    }
    float mean = sum / len;
    return (sum_sq / len) - (mean * mean);
}

static void presence_detection_task(void *pvParameters)
{
    csi_sample_t sample;
    int64_t last_serial_output = 0;

    while (1) {
        if (xQueueReceive(g_csi_queue, &sample, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        // 计算方差
        float variance = compute_variance(sample.data, CSI_SUBCARRIERS);

        // 滑动窗口
        g_variance_window[g_variance_idx] = variance;
        g_variance_idx = (g_variance_idx + 1) % WINDOW_SIZE;
        if (g_variance_idx == 0) g_window_filled = true;

        // 计算窗口内方差的方差（二阶方差）
        int count = g_window_filled ? WINDOW_SIZE : g_variance_idx;
        if (count < 5) continue;

        float mean = 0;
        for (int i = 0; i < count; i++) {
            mean += g_variance_window[i];
        }
        mean /= count;

        float var_of_var = 0;
        for (int i = 0; i < count; i++) {
            float diff = g_variance_window[i] - mean;
            var_of_var += diff * diff;
        }
        var_of_var /= count;

        // 人员检测逻辑
        bool detected = (var_of_var > PRESENCE_THRESHOLD) || (variance > PRESENCE_THRESHOLD * 2);

        if (xSemaphoreTake(g_presence_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_presence.variance = variance;
            bool old_presence = g_presence.presence;
            g_presence.presence = detected;

            // 计算置信度
            float ratio = var_of_var / PRESENCE_THRESHOLD;
            g_presence.confidence = (int)fminf(fmaxf(ratio * 50, 0), 100);

            if (old_presence != detected) {
                g_presence.last_change_ts = sample.timestamp;
            }
            xSemaphoreGive(g_presence_mutex);
        }

        // 串口波形输出（每 100ms 一次）
        int64_t now = esp_timer_get_time();
        if (now - last_serial_output > 100000) {  // 100ms
            last_serial_output = now;
            // 输出格式: $CSI,<variance>,<presence>,<rssi>,<confidence>\n
            printf("$CSI,%.2f,%d,%.0f,%d\n",
                   variance, detected ? 1 : 0, g_presence.rssi, g_presence.confidence);

            // 波形数据输出（子载波幅度）
            printf("$WAV,");
            for (int i = 0; i < CSI_SUBCARRIERS; i++) {
                printf("%d", sample.data[i]);
                if (i < CSI_SUBCARRIERS - 1) printf(",");
            }
            printf("\n");
        }
    }
}

// ========== Web 服务器 ==========
// 嵌入的 HTML 文件
extern const char html_dashboard_start[] asm("_binary_dashboard_html_start");
extern const char html_dashboard_end[]   asm("_binary_dashboard_html_end");

extern const char html_config_start[] asm("_binary_config_html_start");
extern const char html_config_end[]   asm("_binary_config_html_end");

// 仪表盘页面
static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    const size_t html_size = html_dashboard_end - html_dashboard_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_dashboard_start, html_size);
    return ESP_OK;
}

// Captive portal 处理 — 返回 302 重定向到首页，防止手机弹出"需要登录"
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// 配置页面
static esp_err_t config_get_handler(httpd_req_t *req)
{
    const size_t html_size = html_config_end - html_config_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_config_start, html_size);
    return ESP_OK;
}

// 状态 API
static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(g_presence_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"presence\":%s,\"variance\":%.2f,\"rssi\":%.0f,\"confidence\":%d,\"ip\":\"%s\",\"connected\":%s,\"ap_active\":%s,\"csi_count\":%lu}",
             g_presence.presence ? "true" : "false",
             g_presence.variance,
             g_presence.rssi,
             g_presence.confidence,
             g_ip_str,
             g_wifi_connected ? "true" : "false",
             g_ap_active ? "true" : "false",
             (unsigned long)g_csi_count);

    xSemaphoreGive(g_presence_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

// CSI 实时数据 API (短连接，避免阻塞 httpd 线程)
static esp_err_t api_csi_stream_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(g_presence_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    presence_state_t state = g_presence;
    xSemaphoreGive(g_presence_mutex);

    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"variance\":%.2f,\"presence\":%s,\"rssi\":%.0f,\"confidence\":%d,\"connected\":%s,\"ap_active\":%s,\"ip\":\"%s\",\"csi_count\":%lu,\"csi\":[",
        state.variance, state.presence ? "true" : "false", state.rssi, state.confidence,
        g_wifi_connected ? "true" : "false",
        g_ap_active ? "true" : "false",
        g_ip_str,
        (unsigned long)g_csi_count);

    for (int i = 0; i < CSI_SUBCARRIERS && len < 700; i++) {
        len += snprintf(json + len, sizeof(json) - len, "%d%s",
                       g_last_csi.data[i], i < CSI_SUBCARRIERS - 1 ? "," : "");
    }
    len += snprintf(json + len, sizeof(json) - len, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

// WiFi 扫描 API
static esp_err_t api_scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_wifi_scan_stop();
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t ap_records[20];
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    char json[2048];
    int len = snprintf(json, sizeof(json), "{\"aps\":[");
    for (int i = 0; i < ap_count; i++) {
        len += snprintf(json + len, sizeof(json) - len,
                       "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"auth\":%d}",
                       i > 0 ? "," : "",
                       ap_records[i].ssid,
                       ap_records[i].rssi,
                       ap_records[i].primary,
                       ap_records[i].authmode);
    }
    len += snprintf(json + len, sizeof(json) - len, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

// WiFi 配置保存 API
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char buf[256] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 简单解析 JSON: {"ssid":"xxx","password":"xxx"}
    char ssid[MAX_SSID_LEN + 1] = {0};
    char password[MAX_PASSWORD_LEN + 1] = {0};

    char *p = strstr(buf, "\"ssid\":\"");
    if (p) {
        p += strlen("\"ssid\":\"");
        char *end = strchr(p, '"');
        if (end) {
            int slen = end - p;
            if (slen > MAX_SSID_LEN) slen = MAX_SSID_LEN;
            memcpy(ssid, p, slen);
        }
    }

    p = strstr(buf, "\"password\":\"");
    if (p) {
        p += strlen("\"password\":\"");
        char *end = strchr(p, '"');
        if (end) {
            int plen = end - p;
            if (plen > MAX_PASSWORD_LEN) plen = MAX_PASSWORD_LEN;
            memcpy(password, p, plen);
        }
    }

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"SSID is required\"}", 26);
        return ESP_FAIL;
    }

    save_wifi_config(ssid, password);

    // 切换到 STA 模式连接
    wifi_init_sta(ssid, password);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", 14);

    return ESP_OK;
}

// 获取当前配置 API
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"configured\":%s,\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"%s\"}",
             g_wifi_cfg.is_configured ? "true" : "false",
             g_wifi_cfg.ssid,
             g_wifi_connected ? "true" : "false",
             g_ip_str);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = WEB_SERVER_STACK;
    config.max_uri_handlers = 20;
    config.recv_wait_timeout = 10;
    config.max_open_sockets = 4;

    if (httpd_start(&g_server, &config) == ESP_OK) {
        // 页面
        httpd_uri_t dashboard_uri = { .uri = "/", .method = HTTP_GET, .handler = dashboard_get_handler };
        httpd_register_uri_handler(g_server, &dashboard_uri);

        httpd_uri_t config_uri = { .uri = "/config", .method = HTTP_GET, .handler = config_get_handler };
        httpd_register_uri_handler(g_server, &config_uri);

        // Captive portal — 拦截手机/浏览器的"需要登录"检测
        const char *captive_uris[] = {
            "/generate_204", "/gen_204", "/mobile/status.php",
            "/kindle-wifi/wifi/status", "/connectivity-check.html",
            "/check_network_status.txt", "/hotspot-detect.html",
            "/library/test/smile.jpg", "/fwlink", "/redirect",
            "/success.txt", "/ncsi.txt", NULL
        };
        httpd_uri_t captive_uri = { .uri = "", .method = HTTP_GET, .handler = captive_portal_handler };
        for (int i = 0; captive_uris[i]; i++) {
            captive_uri.uri = captive_uris[i];
            httpd_register_uri_handler(g_server, &captive_uri);
        }

        // API
        httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler };
        httpd_register_uri_handler(g_server, &status_uri);

        httpd_uri_t csi_uri = { .uri = "/api/csi", .method = HTTP_GET, .handler = api_csi_stream_handler };
        httpd_register_uri_handler(g_server, &csi_uri);

        httpd_uri_t scan_uri = { .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_get_handler };
        httpd_register_uri_handler(g_server, &scan_uri);

        httpd_uri_t config_post_uri = { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler };
        httpd_register_uri_handler(g_server, &config_post_uri);

        httpd_uri_t config_get_uri = { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler };
        httpd_register_uri_handler(g_server, &config_get_uri);

        ESP_LOGI(TAG, "Web server started");
        return g_server;
    }
    ESP_LOGE(TAG, "Failed to start web server");
    return NULL;
}

// ========== 主函数 ==========
void app_main(void)
{
    ESP_LOGI(TAG, "WiFi Radar - Indoor Human Presence Detection");

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    // 创建互斥锁
    g_presence_mutex = xSemaphoreCreateMutex();

    // 创建 CSI 队列
    g_csi_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_sample_t));

    // 启动 AP 模式（配置用）
    wifi_init_ap();

    ESP_LOGI(TAG, "AP started, loading config...");

    // 加载 WiFi 配置
    load_wifi_config();
    if (g_wifi_cfg.is_configured && strlen(g_wifi_cfg.ssid) > 0) {
        ESP_LOGI(TAG, "Found saved SSID: %s, connecting...", g_wifi_cfg.ssid);
        wifi_init_sta(g_wifi_cfg.ssid, g_wifi_cfg.password);
    } else {
        ESP_LOGI(TAG, "No saved WiFi config, AP-only mode");
    }

    ESP_LOGI(TAG, "Configuring CSI...");

    // 启用 CSI 采集（必须在 esp_wifi_start 之后）
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = true,
        .shift = 0,
    };
    esp_err_t csi_ret;
    csi_ret = esp_wifi_set_csi_config(&csi_config);
    ESP_LOGI(TAG, "CSI config: %s", esp_err_to_name(csi_ret));
    csi_ret = esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL);
    ESP_LOGI(TAG, "CSI callback: %s", esp_err_to_name(csi_ret));
    csi_ret = esp_wifi_set_csi(true);
    ESP_LOGI(TAG, "CSI enable: %s", esp_err_to_name(csi_ret));

    ESP_LOGI(TAG, "CSI collection enabled");

    // 启动 Web 服务器
    ESP_LOGI(TAG, "Starting web server...");
    start_webserver();

    // 创建人员检测任务
    xTaskCreate(presence_detection_task, "presence_detect", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "System ready!");
    ESP_LOGI(TAG, "Configuration page: http://192.168.4.1/config");
    ESP_LOGI(TAG, "Dashboard: http://192.168.4.1/");

    // 主循环：打印状态
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (xSemaphoreTake(g_presence_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Presence: %s | Variance: %.2f | RSSI: %.0f | Confidence: %d%% | IP: %s | CSI: %lu",
                    g_presence.presence ? "OCCUPIED" : "EMPTY",
                    g_presence.variance,
                    g_presence.rssi,
                    g_presence.confidence,
                    g_ip_str,
                    (unsigned long)g_csi_count);
            xSemaphoreGive(g_presence_mutex);
        }
    }
}
