#pragma once

// ============================================================================
// web_server.hpp - Web UI giám sát + Admin panel
// ----------------------------------------------------------------------------
//   GET /                  -> trang xem stream + số liệu
//   GET /stream            -> MJPEG stream
//   GET /capture           -> 1 ảnh JPEG
//   GET /status            -> JSON metrics realtime
//   GET /admin             -> trang quản trị (mật khẩu DROWSY_ADMIN_PASS)
//   GET /admin/api/config  -> xem/đổi cấu hình (lưu NVS, sống qua reboot)
//   GET /admin/api/reboot  -> khởi động lại thiết bị
//   GET /admin/api/log     -> vài dòng log gần nhất
//
// WiFi mode: AP (mặc định - demo) / STA / OFF - chọn trong menuconfig
// ============================================================================

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "config_store.hpp"
#include "drowsiness_detector.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Số liệu chia sẻ giữa AI task (ghi) và web /status (đọc) ----
typedef struct {
    int state;          // DrowsyState
    float ear;
    float mar;
    float pitch;
    float pitch_dev;
    float perclos;      // 0..1
    float blink_rate;   // nhịp/phút
    float yawn_rate;    // lần/phút
    float fatigue;      // 0..1
    float fps;
    int face;           // 1 = có khuôn mặt
    int metrics_ok;     // 1 = landmark hợp lệ
} web_metrics_t;

/**
 * @brief Khởi tạo web: WiFi (AP/STA/OFF) + mDNS + httpd + task drain frame.
 * @param frame_i  Queue nhận camera_fb_t* từ AI task (web task sẽ fb_return).
 * @param metrics  Con trỏ metrics (AI task cập nhật mỗi frame).
 * @param detector Con trỏ detector (admin đổi ngưỡng runtime).
 * @param config   Con trỏ cấu hình NVS (admin xem/sửa).
 */
void web_server_init(QueueHandle_t frame_i,
                     web_metrics_t *metrics,
                     DrowsinessDetector *detector,
                     device_config_t *config);

/**
 * @brief Đăng ký queue alarm để app gửi cảnh báo qua POST /api/alarm
 *        (phương án AI offload: điện thoại tự tính state, ESP32 chỉ bật
 *        buzzer/LED theo lệnh app).
 * @param q Queue nhận alarm_event_t (từ AlarmControl::init()).
 */
void web_server_set_alarm_queue(QueueHandle_t q);

/**
 * @brief Đẩy 1 dòng vào ring log của web (phục vụ /admin/api/log).
 */
void web_log_line(const char *fmt, ...);

/**
 * @brief Khoá/ mở khoá metrics (AI task Core 1 ghi, httpd/report Core 0 đọc).
 * Dùng để copy struct một cách an toàn, tránh torn read (fix M2).
 */
void web_metrics_lock(void);
void web_metrics_unlock(void);

/**
 * @brief Vẽ overlay lên frame RGB565: khung mặt + 98 landmark + text metrics.
 */
void web_draw_overlay(camera_fb_t *fb,
                      const int *face_box,
                      const float *landmarks,
                      bool face_ok,
                      const web_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
