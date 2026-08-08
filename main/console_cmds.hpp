#pragma once

// ============================================================================
// console_cmds.hpp - Lệnh console runtime: drowsy get/set/stats/reset
// ============================================================================

#include "drowsiness_detector.hpp"
#include "config_store.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Đăng ký lệnh `drowsy` với esp_console.
 *
 * Cách dùng (qua `idf.py monitor`):
 *   drowsy get                      -> in toàn bộ ngưỡng + trạng thái hiện tại
 *   drowsy set ear_blink_th 0.22    -> chỉnh ngưỡng runtime + LƯU NVS (fix C5)
 *   drowsy stats                    -> in PERCLOS, blink/min, yawn/min, fatigue
 *   drowsy reset                    -> reset state machine + baseline pitch
 */
void register_drowsy_commands(DrowsinessDetector *detector, device_config_t *config);

#ifdef __cplusplus
}
#endif
