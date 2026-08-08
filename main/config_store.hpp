#pragma once

// ============================================================================
// config_store.hpp - Lưu cấu hình thiết bị vào NVS (sống qua reboot)
// ----------------------------------------------------------------------------
// Dùng cho admin web panel: đổi ngưỡng / WiFi / tên thiết bị rồi lưu NVS.
// Nếu chưa có cấu hình trong NVS -> dùng giá trị mặc định từ Kconfig.
// ============================================================================

#include <stdint.h>

#include "esp_err.h"

#define CFG_WIFI_SSID_MAX 33
#define CFG_WIFI_PASS_MAX 65
#define CFG_NAME_MAX      33

#define CFG_VERSION 3 // v3 (fix A4): lat/lng thêm sau - buộc reset config cũ 1 lần

typedef struct {
    uint32_t version; // CFG_VERSION - để migrate sau này
    char device_name[CFG_NAME_MAX];
    char device_id[CFG_NAME_MAX];      // định danh báo lên server (rỗng = từ MAC)
    char driver_name[CFG_NAME_MAX];    // tài xế
    char location[CFG_NAME_MAX];       // địa chỉ xe
    double lat;                        // vĩ độ (GPS - hiện đặt tay qua admin)
    double lng;                        // kinh độ
    char wifi_ssid[CFG_WIFI_SSID_MAX];
    char wifi_pass[CFG_WIFI_PASS_MAX];
    int32_t wifi_mode;        // 0=AP, 1=STA, 2=OFF
    // Ngưỡng (đơn vị x1000 như Kconfig; 0 = chưa đặt, dùng mặc định)
    int32_t ear_blink_th_x1000;
    int32_t ear_drowsy_th_x1000;
    int32_t mar_th_x1000;
    int32_t perclos_th_x1000; // 400 = 40%
    int32_t pitch_dev_th_x1000;
    int32_t microsleep_ms;
    int32_t window_ms;
    int32_t blink_rate_high;
} device_config_t;

/**
 * @brief Điền cấu hình mặc định (từ Kconfig).
 */
void config_fill_defaults(device_config_t *cfg);

/**
 * @brief Nạp cấu hình từ NVS; nếu chưa có -> trả về mặc định Kconfig.
 * @note  Yêu cầu nvs_flash_init() đã chạy.
 */
esp_err_t config_store_load(device_config_t *cfg);

/**
 * @brief Ghi cấu hình vào NVS.
 */
esp_err_t config_store_save(const device_config_t *cfg);

/**
 * @brief Xoá cấu hình NVS (về mặc định Kconfig).
 */
esp_err_t config_store_reset(void);

#ifdef __cplusplus
extern "C" {
#endif
#ifdef __cplusplus
}
#endif
