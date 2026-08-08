// ============================================================================
// config_store.cpp - Lưu cấu hình thiết bị vào NVS
// ============================================================================

#include "config_store.hpp"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "config_store";

#define NVS_NAMESPACE "drowsy"
#define NVS_KEY_CFG   "cfg"

void config_fill_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = CFG_VERSION;
    snprintf(cfg->device_name, sizeof(cfg->device_name), "Drowsy-Driver-Monitor");
    // LƯU Ý: các option này depends on STA+REPORT - nếu bị ẩn (AP mode) thì không
    // có macro -> phải dùng #if defined() để tránh lỗi biên dịch
#if defined(CONFIG_DROWSY_DEVICE_ID)
    snprintf(cfg->device_id, sizeof(cfg->device_id), "%s", CONFIG_DROWSY_DEVICE_ID);
#endif
#if defined(CONFIG_DROWSY_DRIVER_NAME)
    snprintf(cfg->driver_name, sizeof(cfg->driver_name), "%s", CONFIG_DROWSY_DRIVER_NAME);
#endif
#if defined(CONFIG_DROWSY_LOCATION)
    snprintf(cfg->location, sizeof(cfg->location), "%s", CONFIG_DROWSY_LOCATION);
#endif
#if CONFIG_DROWSY_WIFI_MODE_AP
    cfg->wifi_mode = 0;
    snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", CONFIG_DROWSY_AP_SSID);
    snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", CONFIG_DROWSY_AP_PASS);
#elif CONFIG_DROWSY_WIFI_MODE_STA
    cfg->wifi_mode = 1;
    snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", CONFIG_DROWSY_WIFI_SSID);
    snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", CONFIG_DROWSY_WIFI_PASS);
#else
    cfg->wifi_mode = 2;
#endif
    // Ngưỡng (x1000)
    cfg->ear_blink_th_x1000 = CONFIG_DROWSY_EAR_BLINK_TH;
    cfg->ear_drowsy_th_x1000 = CONFIG_DROWSY_EAR_DROWSY_TH;
    cfg->mar_th_x1000 = CONFIG_DROWSY_MAR_TH;
    cfg->perclos_th_x1000 = CONFIG_DROWSY_PERCLOS_TH;
    cfg->pitch_dev_th_x1000 = CONFIG_DROWSY_PITCH_DEV_TH;
    cfg->microsleep_ms = CONFIG_DROWSY_MICROSLEEP_MS;
    cfg->window_ms = CONFIG_DROWSY_WINDOW_MS;
    cfg->blink_rate_high = CONFIG_DROWSY_BLINK_RATE_HIGH;
}

esp_err_t config_store_load(device_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        config_fill_defaults(cfg);
        return ESP_OK; // chưa có cấu hình -> dùng mặc định
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open fail: %s", esp_err_to_name(err));
        config_fill_defaults(cfg);
        return err;
    }

    // QUAN TRỌNG (fix A4): memset TRƯỚC nvs_get_blob - nếu blob CŨ nhỏ hơn struct
    // mới (thêm field nhưng quên tăng version), nvs_get_blob chỉ copy phần đầu ->
    // phần thừa = garbage data (lat/lng/fields mới = số rác).
    memset(cfg, 0, sizeof(*cfg));
    size_t len = sizeof(device_config_t);
    err = nvs_get_blob(h, NVS_KEY_CFG, cfg, &len);
    nvs_close(h);
    if (err != ESP_OK || cfg->version != CFG_VERSION) {
        config_fill_defaults(cfg); // sai version/hỏng -> về mặc định
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t config_store_save(const device_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(rw) fail: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_CFG, cfg, sizeof(device_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved (%d bytes)", (int)sizeof(device_config_t));
    return err;
}

esp_err_t config_store_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, NVS_KEY_CFG);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "Config reset -> về mặc định Kconfig");
    return err;
}
