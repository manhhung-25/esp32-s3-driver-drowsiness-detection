#pragma once

// ============================================================================
// report_client.hpp - Báo trạng thái lên Admin Server (đa thiết bị)
// ----------------------------------------------------------------------------
// Task mỗi 5s (chỉ khi WiFi STA + DROWSY_REPORT_ENABLE):
//   POST /api/report (JSON: device_id, driver_name, location, metrics...)
//   -> server trả về lệnh đang chờ (reboot / set ngưỡng) -> thực thi
// ============================================================================

#include "config_store.hpp"
#include "drowsiness_detector.hpp"
#include "web_server.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo task báo trạng thái lên server.
 * @param cfg      Cấu hình thiết bị (device_id/driver_name/location/ngưỡng)
 * @param metrics  Số liệu realtime (AI task cập nhật)
 * @param detector Detector (để áp lệnh đổi ngưỡng từ server)
 */
void report_client_start(device_config_t *cfg, web_metrics_t *metrics, DrowsinessDetector *detector);

#ifdef __cplusplus
}
#endif
