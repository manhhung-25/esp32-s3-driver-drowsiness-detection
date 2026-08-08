// ============================================================================
// report_client.cpp - Báo trạng thái lên Admin Server (đa thiết bị)
// ============================================================================

// QUAN TRỌNG: include sdkconfig.h TRƯỚC #if (giống web_server.cpp) - nếu không
// CONFIG_DROWSY_* undefined -> report client bị compile thành rỗng.
#include "sdkconfig.h"

#if CONFIG_DROWSY_REPORT_ENABLE && CONFIG_DROWSY_WIFI_MODE_STA

#include "report_client.hpp"

#include <stdio.h>
#include <stdlib.h> // atol
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "report_client";

static device_config_t *s_cfg = NULL;
static web_metrics_t *s_metrics = NULL;
static DrowsinessDetector *s_det = NULL;

static void apply_server_payload(const char *payload); // forward decl

// ---- Tạo device_id mặc định từ MAC nếu Kconfig để trống ----
static const char *get_device_id(void)
{
    static char dev_id[32] = "";
    if (s_cfg->device_id[0] != 0) {
        return s_cfg->device_id;
    }
    if (dev_id[0] == 0) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(dev_id, sizeof(dev_id), "S3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    return dev_id;
}

// ---- Gửi JSON trạng thái + nhận lệnh ----
static void post_report(void)
{
    char body[512];
    // fix M2: copy metrics dưới spinlock (AI task Core 1 ghi) tránh torn read
    web_metrics_t m = {};
    web_metrics_lock();
    if (s_metrics) m = *s_metrics;
    web_metrics_unlock();
    snprintf(body, sizeof(body),
             "{\"device_id\":\"%s\",\"driver_name\":\"%s\",\"location\":\"%s\","
             "\"lat\":%.5f,\"lng\":%.5f,"
             "\"state_id\":%d,\"ear\":%.3f,\"mar\":%.3f,\"pitch_dev\":%.3f,\"perclos\":%.1f,"
             "\"blink_min\":%.1f,\"yawn_min\":%.1f,\"fatigue\":%.2f,\"fps\":%.1f,"
             "\"face\":%d,\"metrics_ok\":%d,\"uptime_s\":%u}",
             get_device_id(), s_cfg->driver_name, s_cfg->location,
             s_cfg->lat, s_cfg->lng,
             m.state, m.ear, m.mar, m.pitch_dev,
             m.perclos, m.blink_rate, m.yawn_rate,
             m.fatigue, m.fps, m.face, m.metrics_ok,
             (unsigned)(esp_timer_get_time() / 1000000));

    esp_http_client_config_t cfg = {
        .url = CONFIG_DROWSY_REPORT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        // Đọc phản hồi (JSON lệnh): {"command":"reboot"} hoặc {"command":"set","payload":"k=v&..."}
        char resp[160] = "";
        int len = esp_http_client_read_response(client, resp, sizeof(resp) - 1);
        if (len > 0) {
            resp[len] = 0;
            if (strstr(resp, "reboot") != NULL) {
                ESP_LOGW(TAG, "Nhận lệnh REBOOT từ server - khởi động lại!");
                esp_restart();
            } else if (strstr(resp, "set") != NULL) {
                ESP_LOGI(TAG, "Nhận lệnh SET: %s", resp);
                // payload dạng "k=v&k=v..." - tách từ '"payload":"..."'
                const char *p = strstr(resp, "payload");
                if (p) {
                    char *start = strchr(p, '"');
                    char *end = strrchr(p, '"');
                    if (start && end && end > start) {
                        *end = 0;
                        apply_server_payload(start + 1);
                    }
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "Report lên server thất bại: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

// ---- Áp payload "k=v&k=v" (đơn vị x1000 cho tỷ lệ) vào cấu hình + detector ----
static void apply_server_payload(const char *payload)
{
    if (s_cfg == NULL || s_det == NULL) return;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", payload);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "&", &save); tok != NULL; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = tok;
        long val = atol(eq + 1);
        if      (strcmp(key, "ear_blink_th") == 0)    s_cfg->ear_blink_th_x1000 = (int32_t)val;
        else if (strcmp(key, "ear_drowsy_th") == 0)   s_cfg->ear_drowsy_th_x1000 = (int32_t)val;
        else if (strcmp(key, "mar_th") == 0)          s_cfg->mar_th_x1000 = (int32_t)val;
        else if (strcmp(key, "perclos_th") == 0)      s_cfg->perclos_th_x1000 = (int32_t)val;
        else if (strcmp(key, "pitch_dev_th") == 0)    s_cfg->pitch_dev_th_x1000 = (int32_t)val;
        else if (strcmp(key, "microsleep_ms") == 0)   s_cfg->microsleep_ms = (int32_t)val;
        else if (strcmp(key, "window_ms") == 0)       s_cfg->window_ms = (int32_t)val;
        else if (strcmp(key, "blink_rate_high") == 0) s_cfg->blink_rate_high = (int32_t)val;
    }
    config_store_save(s_cfg);
    DrowsyParams p = s_det->params();
    p.ear_blink_th = s_cfg->ear_blink_th_x1000 / 1000.0f;
    p.ear_drowsy_th = s_cfg->ear_drowsy_th_x1000 / 1000.0f;
    p.mar_th = s_cfg->mar_th_x1000 / 1000.0f;
    p.perclos_th = s_cfg->perclos_th_x1000 / 1000.0f;
    p.pitch_dev_th = s_cfg->pitch_dev_th_x1000 / 1000.0f;
    p.microsleep_ms = (uint32_t)s_cfg->microsleep_ms;
    p.window_ms = (uint32_t)s_cfg->window_ms;
    p.blink_rate_high = (uint32_t)s_cfg->blink_rate_high;
    s_det->apply_params(p);
    ESP_LOGI(TAG, "Đã áp ngưỡng mới từ server (lưu NVS)");
}

static void report_task(void *arg)
{
    while (true) {
        post_report();
        vTaskDelay(pdMS_TO_TICKS(5000)); // mỗi 5s
    }
}

void report_client_start(device_config_t *cfg, web_metrics_t *metrics, DrowsinessDetector *detector)
{
    s_cfg = cfg;
    s_metrics = metrics;
    s_det = detector;
    ESP_LOGI(TAG, "Bắt đầu báo trạng thái: %s (mỗi 5s)", CONFIG_DROWSY_REPORT_URL);
    xTaskCreatePinnedToCore(report_task, "report", 4 * 1024, NULL, 4, NULL, 0);
}

#endif // CONFIG_DROWSY_REPORT_ENABLE && CONFIG_DROWSY_WIFI_MODE_STA
