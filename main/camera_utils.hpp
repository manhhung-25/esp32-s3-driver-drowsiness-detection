#pragma once

// ============================================================================
// camera_utils.hpp - Khởi tạo camera (esp32-camera)
// ----------------------------------------------------------------------------
// LƯU Ý QUAN TRỌNG: esp32-camera v2.1.x đã GỠ BỎ các option menuconfig
// CONFIG_CAMERA_MODULE_* / CONFIG_CAMERA_PIN_*. Do đó project này tự định
// nghĩa pin qua Kconfig.projbuild (DROWSY_CAM_*) và gán trực tiếp vào
// camera_config_t - hoạt động trên MỌI version esp32-camera, MỌI board
// (OV2640/OV5640/...).
// ============================================================================

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo camera và tạo task đẩy frame vào queue.
 *
 * @param pixel_format PIXFORMAT_RGB565 (khuyên dùng cho AI) hoặc PIXFORMAT_JPEG
 * @param frame_size   FRAMESIZE_240X240 (mặc định) hoặc FRAMESIZE_QVGA
 * @param fb_count     Số frame buffer (2 = double buffer, tăng FPS)
 * @param frame_o      Queue nhận camera_fb_t* (size khuyên dùng 3)
 * @return true nếu camera khởi tạo thành công
 */
bool register_camera(const pixformat_t pixel_format,
                     const framesize_t frame_size,
                     const uint8_t fb_count,
                     const QueueHandle_t frame_o);

#ifdef __cplusplus
}
#endif
