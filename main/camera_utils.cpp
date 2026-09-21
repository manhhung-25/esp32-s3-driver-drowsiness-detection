// ============================================================================
// camera_utils.cpp - Khởi tạo camera với pin cấu hình qua Kconfig riêng
// (Adapt từ who_camera.c của ESP-WHO - MIT License, Espressif Systems)
// ============================================================================

#include "camera_utils.hpp"
#include "web_server.hpp"

#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "camera_utils";

static QueueHandle_t s_ai_frame_queue = NULL;
static QueueHandle_t s_stream_frame_queue = NULL;

static void queue_latest(QueueHandle_t queue, camera_fb_t *frame)
{
    if (queue == NULL) {
        esp_camera_fb_return(frame);
        return;
    }
    if (xQueueSend(queue, &frame, 0) == pdTRUE) return;

    camera_fb_t *stale = NULL;
    if (xQueueReceive(queue, &stale, 0) == pdTRUE && stale != NULL) {
        esp_camera_fb_return(stale);
    }
    if (xQueueSend(queue, &frame, 0) != pdTRUE) {
        esp_camera_fb_return(frame);
    }
}

// ---- Map preset board -> pin (dựa trên esp32-camera v2.0.x who_camera.h) ----
#if CONFIG_DROWSY_CAM_MODULE_ESP32_S3_EYE
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   15
#define CAM_PIN_SIOD   4
#define CAM_PIN_SIOC   5
#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2     8
#define CAM_PIN_D1     9
#define CAM_PIN_D0     11
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF   7
#define CAM_PIN_PCLK   13
#elif CONFIG_DROWSY_CAM_MODULE_AI_THINKER
#define CAM_PIN_PWDN   32
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   0
#define CAM_PIN_SIOD   26
#define CAM_PIN_SIOC   27
#define CAM_PIN_D7     35
#define CAM_PIN_D6     34
#define CAM_PIN_D5     39
#define CAM_PIN_D4     36
#define CAM_PIN_D3     21
#define CAM_PIN_D2     19
#define CAM_PIN_D1     18
#define CAM_PIN_D0     5
#define CAM_PIN_VSYNC  25
#define CAM_PIN_HREF   23
#define CAM_PIN_PCLK   22
#else // CONFIG_DROWSY_CAM_MODULE_CUSTOM
#define CAM_PIN_PWDN   CONFIG_DROWSY_CAM_PIN_PWDN
#define CAM_PIN_RESET  CONFIG_DROWSY_CAM_PIN_RESET
#define CAM_PIN_XCLK   CONFIG_DROWSY_CAM_PIN_XCLK
#define CAM_PIN_SIOD   CONFIG_DROWSY_CAM_PIN_SIOD
#define CAM_PIN_SIOC   CONFIG_DROWSY_CAM_PIN_SIOC
#define CAM_PIN_D7     CONFIG_DROWSY_CAM_PIN_Y9
#define CAM_PIN_D6     CONFIG_DROWSY_CAM_PIN_Y8
#define CAM_PIN_D5     CONFIG_DROWSY_CAM_PIN_Y7
#define CAM_PIN_D4     CONFIG_DROWSY_CAM_PIN_Y6
#define CAM_PIN_D3     CONFIG_DROWSY_CAM_PIN_Y5
#define CAM_PIN_D2     CONFIG_DROWSY_CAM_PIN_Y4
#define CAM_PIN_D1     CONFIG_DROWSY_CAM_PIN_Y3
#define CAM_PIN_D0     CONFIG_DROWSY_CAM_PIN_Y2
#define CAM_PIN_VSYNC  CONFIG_DROWSY_CAM_PIN_VSYNC
#define CAM_PIN_HREF   CONFIG_DROWSY_CAM_PIN_HREF
#define CAM_PIN_PCLK   CONFIG_DROWSY_CAM_PIN_PCLK
#endif

#define XCLK_FREQ_HZ 20000000 // 20MHz cho OV2640 tăng tốc DMA capture

// Task đẩy frame từ camera vào queue (pinned Core 0 - camera DMA/D2D)
static void camera_task(void *arg)
{
    uint32_t frame_number = 0;
    while (true) {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame) {
            // Queue đầy -> chờ (backpressure tự nhiên, không drop đột ngột)
            web_note_camera_frame();
            const bool sample_for_ai = (frame_number++ % CONFIG_DROWSY_AI_SAMPLE_EVERY_N) == 0;
            if (sample_for_ai && xQueueSend(s_ai_frame_queue, &frame, 0) == pdTRUE) {
                continue;
            }

            web_draw_cached_overlay(frame);
            queue_latest(s_stream_frame_queue, frame);
        }
    }
}

bool register_camera(const pixformat_t pixel_format,
                     const framesize_t frame_size,
                     const uint8_t fb_count,
                     const QueueHandle_t ai_frame_o,
                     const QueueHandle_t stream_frame_o)
{
    // ---- ESP32-CAM board: GPIO13/14 mặc định là JTAG, phải chuyển sang input ----
#if CONFIG_DROWSY_CAM_MODULE_AI_THINKER
    gpio_config_t conf = {};
    conf.mode = GPIO_MODE_INPUT;
    conf.pull_up_en = GPIO_PULLUP_ENABLE;
    conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    conf.intr_type = GPIO_INTR_DISABLE;
    conf.pin_bit_mask = (1ULL << 13) | (1ULL << 14);
    gpio_config(&conf);
#endif

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_sscb_sda = CAM_PIN_SIOD;
    config.pin_sscb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.pixel_format = pixel_format;
    config.frame_size = frame_size;
    config.jpeg_quality = 12;
    config.fb_count = fb_count;
    config.fb_location = CAMERA_FB_IN_PSRAM; // N16R8: 8MB Octal PSRAM - buffer ở PSRAM
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x. Kiểm tra pin trong menuconfig!", err);
        return false;
    }

    // ---- Chỉnh sensor theo loại (giống who_camera.c) ----
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        // QUAN TRỌNG: thêm OV5640 - board của bạn dùng OV5640 (log: Detected OV5640).
        // Nếu không vflip, ảnh LỘN NGƯỢC -> face detect không tìm thấy mặt -> face=0
        // -> EAR=0.00 (nguyên nhân gốc của "không nhận diện được" từ đầu!)
        if (s->id.PID == OV3660_PID || s->id.PID == OV2640_PID || s->id.PID == OV5640_PID) {
            s->set_vflip(s, 1); // lật ngược ảnh cho đúng chiều
        } else if (s->id.PID == GC0308_PID) {
            s->set_hmirror(s, 0);
        } else if (s->id.PID == GC032A_PID) {
            s->set_vflip(s, 1);
        }
        if (s->id.PID == OV3660_PID) {
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
        }
    }

    s_ai_frame_queue = ai_frame_o;
    s_stream_frame_queue = stream_frame_o;
    // Task camera trên Core 0 - AI pipeline chạy Core 1, không cạnh tranh nhau
    xTaskCreatePinnedToCore(camera_task, "camera_task", 3 * 1024, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Camera ready.");
    return true;
}
