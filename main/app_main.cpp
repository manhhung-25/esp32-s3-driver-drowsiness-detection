// ============================================================================
// app_main.cpp - Driver Drowsiness Detection (ESP32-S3-N16R8)
// ----------------------------------------------------------------------------
// Pipeline (dual-core):
//   Core 0: camera_task  -> esp_camera_fb_get() -> queue frame (size 3)
//   Core 1: ai_task      -> face detect (MSR+MNP) -> PFLD 98 landmark
//                          -> EAR/MAR/pitch -> state machine -> alarm queue
//   Core 1: alarm_task   -> buzzer (LEDC PWM) + LED theo pattern
//
// Console (USB-Serial/JTAG): `drowsy get/set/stats/reset`
// ============================================================================

#include <cstring>
#include <vector>

#include "esp_camera.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dl_image_define.hpp"

#include "alarm_control.hpp"
#include "benchmark.hpp"
#include "camera_utils.hpp"
#include "config_store.hpp"
#include "console_cmds.hpp"
#include "drowsiness_detector.hpp"
#include "face_pipeline.hpp"
#include "landmark_pfld.hpp"
#include "nvs_flash.h"
#include "report_client.hpp"
#include "web_server.hpp"

static const char *TAG = "app_main";

static QueueHandle_t s_frame_queue = NULL;
static QueueHandle_t s_alarm_queue = NULL;
#if CONFIG_DROWSY_WEB_ENABLE
static QueueHandle_t s_web_frame_queue = NULL;
static web_metrics_t s_web_metrics = {};
#endif
static device_config_t s_device_config; // cấu hình NVS (admin panel)
#if CONFIG_DROWSY_AI_ON_DEVICE
static DrowsyState s_last_state = DrowsyState::AWAKE;
static uint64_t s_state_since_ms = 0;

static const char *drowsy_state_name(DrowsyState state)
{
    switch (state) {
    case DrowsyState::AWAKE:      return "AWAKE";
    case DrowsyState::PRE_DROWSY: return "PRE_DROWSY";
    case DrowsyState::DROWSY:     return "DROWSY";
    case DrowsyState::MICROSLEEP: return "MICROSLEEP";
    case DrowsyState::DISTRACTED: return "DISTRACTED";
    default:                      return "UNKNOWN";
    }
}
#endif

#if CONFIG_DROWSY_AI_ON_DEVICE
static FacePipeline s_face;
static LandmarkPFLD s_pfld;
#endif
static DrowsinessDetector s_detector;
static AlarmControl s_alarm;

#if CONFIG_DROWSY_AI_ON_DEVICE
static float face_box_iou(const std::vector<int> &box, const int cached_box[4])
{
    if (box.size() < 4) return 0.0f;

    const int ix1 = box[0] > cached_box[0] ? box[0] : cached_box[0];
    const int iy1 = box[1] > cached_box[1] ? box[1] : cached_box[1];
    const int ix2 = box[2] < cached_box[2] ? box[2] : cached_box[2];
    const int iy2 = box[3] < cached_box[3] ? box[3] : cached_box[3];
    const int iw = ix2 > ix1 ? ix2 - ix1 : 0;
    const int ih = iy2 > iy1 ? iy2 - iy1 : 0;
    const int intersection = iw * ih;
    const int area_a = (box[2] - box[0]) * (box[3] - box[1]);
    const int area_b = (cached_box[2] - cached_box[0]) * (cached_box[3] - cached_box[1]);
    const int total = area_a + area_b - intersection;
    return total > 0 ? (float)intersection / (float)total : 0.0f;
}
#endif

// ---- Áp cấu hình NVS vào detector (ngưỡng) - lúc boot ----
static void apply_config_to_detector(void)
{
    DrowsyParams p = s_detector.params();
    p.ear_blink_th = s_device_config.ear_blink_th_x1000 / 1000.0f;
    p.ear_drowsy_th = s_device_config.ear_drowsy_th_x1000 / 1000.0f;
    p.mar_th = s_device_config.mar_th_x1000 / 1000.0f;
    p.perclos_th = s_device_config.perclos_th_x1000 / 1000.0f;
    p.pitch_dev_th = s_device_config.pitch_dev_th_x1000 / 1000.0f;
    p.microsleep_ms = (uint32_t)s_device_config.microsleep_ms;
    p.window_ms = (uint32_t)s_device_config.window_ms;
    p.blink_rate_high = (uint32_t)s_device_config.blink_rate_high;
    s_detector.apply_params(p);
}

#if CONFIG_DROWSY_AI_ON_DEVICE
// ----------------------------------------------------------------------------
// AI task (Core 1): xử lý toàn bộ pipeline nhận diện + state machine
// ----------------------------------------------------------------------------
static void ai_task(void *arg)
{
    uint32_t frames = 0;
    uint64_t t0_us = esp_timer_get_time();
    const uint32_t log_interval_ms = CONFIG_DROWSY_FPS_LOG_INTERVAL_S * 1000u;
    int face_ms_last = 0, pfld_ms_last = 0;      // timing chẩn đoán
    int wait_ms_last = 0, loop_ms_last = 0;      // chờ frame + tổng vòng lặp
    int px_r_last = 0, px_b_last = 0;            // pixel giữa (chẩn đoán byte-order)
    int avg_r_last = 0, avg_b_last = 0;          // trung bình R/B toàn khung (theo LE)
    int msr_cnt_last = 0, mnp_cnt_last = 0;      // số candidate (face=0 debug)

    while (true) {
        camera_fb_t *fb = NULL;
        int64_t t_loop = esp_timer_get_time();
        if (xQueueReceive(s_frame_queue, &fb, portMAX_DELAY) != pdTRUE || fb == NULL) {
            continue;
        }
        wait_ms_last = (int)((esp_timer_get_time() - t_loop) / 1000); // thời gian chờ camera
        // fix M5: uint64_t (tránh tràn 32-bit sau ~49,7 ngày uptime)
        const uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);

        // Pixel GIỮA + TRUNG BÌNH R/B toàn khung (theo cách esp-dl LE đọc):
        // da người -> R > B. Nếu avg_b > avg_r rõ rệt -> RAW là BIG-ENDIAN
        // (esp-dl LE đang đảo R/B) -> bật CONFIG_DROWSY_CAM_RGB565_BE.
        {
            uint64_t sum_r = 0, sum_b = 0;
            uint32_t npx = (uint32_t)fb->width * fb->height;
            uint16_t *p = (uint16_t *)fb->buf;
            for (uint32_t i = 0; i < npx; ++i) {
                sum_r += (p[i] >> 11) & 0x1F;
                sum_b += p[i] & 0x1F;
            }
            avg_r_last = (int)(sum_r / npx);
            avg_b_last = (int)(sum_b / npx);
            uint16_t center = p[(fb->height / 2) * fb->width + (fb->width / 2)];
            px_r_last = (center >> 11) & 0x1F;
            px_b_last = center & 0x1F;
        }

        // Wrap frame RGB565 thành dl::image::img_t (không copy - zero-copy)
        // Kconfig DROWSY_CAM_RGB565_BE: thử khi face=0 do màu bị đảo (da xanh/tím)
        dl::image::img_t img;
#if CONFIG_DROWSY_CAM_RGB565_BE
        img = {(void *)fb->buf, (uint16_t)fb->width, (uint16_t)fb->height,
               dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
#else
        img = {(void *)fb->buf, (uint16_t)fb->width, (uint16_t)fb->height,
               dl::image::DL_IMAGE_PIX_TYPE_RGB565LE};
#endif

        // ---- 1. Face detection (MSR + MNP) - đo thời gian để chẩn đoán FPS ----
        int64_t t1 = esp_timer_get_time();
        std::vector<int> face_box, keypoints;
        static std::vector<int> s_face_box_cache;
        static int s_face_detect_skip = 0;
        static int s_face_miss_skip = 0;
        const bool run_face_detect = s_face_box_cache.empty()
                                         ? (s_face_miss_skip++ % 2 == 0)
                                         : (s_face_detect_skip++ % 4 == 0);
        bool has_face = false;
        if (run_face_detect) {
            has_face = s_face.detect(img, face_box, keypoints, &msr_cnt_last, &mnp_cnt_last);
            face_ms_last = (int)((esp_timer_get_time() - t1) / 1000);
            if (has_face) {
                s_face_box_cache = face_box;
            } else {
                s_face_box_cache.clear();
            }
        } else {
            has_face = true;
            face_box = s_face_box_cache;
            face_ms_last = 0;
            msr_cnt_last = 0;
            mnp_cnt_last = 0;
        }

        // ---- 2. Landmark PFLD + EAR/MAR/pitch ----
        // PFLD mất ~519ms -> chạy CÁCH QUÃNG (mỗi 2 frame), frame xen kẽ dùng
        // metric CACHE (EAR không bị reset 0 -> state machine vẫn đúng).
        // fix B3: khi skip nhưng CHƯA có cache hợp lệ (frame đầu) -> vẫn chạy PFLD
        // để có metric ngay (tránh mất mẫu đầu -> mất cảnh báo đầu tiên).
        static FaceMetrics s_metrics_cache;
        static float s_landmarks_cache[PFLD_NUM_POINTS * 2] = {};
        static int s_landmark_face_box[4] = {};
        static bool s_metrics_valid = false;
        static bool s_landmarks_valid = false;
        static int s_pfld_skip = 0;
        FaceMetrics m;
        bool metrics_ok = false;
        float landmarks[PFLD_NUM_POINTS * 2];
        bool pfld_ran = false;
        int64_t t2 = esp_timer_get_time();
        // fix B1 (tăng FPS): chạy PFLD mỗi 3 frame thay vì 2 -> AI ~3 FPS, bớt tải.
        // (Vẫn giữ !s_metrics_valid để có metric ngay ở frame đầu.)
        // fix NEW-3: khi MẤT mặt -> reset cache để face quay lại không dùng metric cũ
        if (!has_face) {
            s_metrics_valid = false;
            s_landmarks_valid = false;
            s_face_box_cache.clear();
        } else if (s_landmarks_valid && face_box_iou(face_box, s_landmark_face_box) < 0.45f) {
            // A new face or detector jump must not inherit old landmarks.
            s_metrics_valid = false;
            s_landmarks_valid = false;
        }
        // fix NEW-1: khi EAR cache thấp (đang nhắm mắt - nghi microsleep) -> chạy
        // PFLD mỗi frame để bắt chính xác thời điểm nhắm (không trễ 1s vì skip).
        const int pfld_period = (s_detector.eyes_closed() || s_detector.yawning()) ? 2 : 3;
        bool run_pfld = (s_pfld_skip++ % pfld_period == 0) || !s_metrics_valid;
        if (has_face && run_pfld &&
            s_pfld.run(img, face_box, landmarks)) {
            pfld_ran = true; // PFLD thực sự chạy frame này
            if (compute_face_metrics(landmarks, &m)) {
                s_metrics_cache = m;
                s_metrics_valid = true;
                std::memcpy(s_landmarks_cache, landmarks, sizeof(s_landmarks_cache));
                for (int i = 0; i < 4; ++i) {
                    s_landmark_face_box[i] = face_box[i];
                }
                s_landmarks_valid = true;
                metrics_ok = true;
            }
        } else if (has_face && s_metrics_valid) {
            m = s_metrics_cache; // frame skip: dùng metric gần nhất
            metrics_ok = true;
        }
        // Chỉ đo pfld_ms trên frame PFLD THỰC SỰ chạy (frame skip giữ giá trị cũ)
        if (pfld_ran) {
            pfld_ms_last = (int)((esp_timer_get_time() - t2) / 1000);
        }
        // Không có mặt HOẶC landmark rác (compute_face_metrics trả false vì
        // nghiêng quá 40° / sai thứ tự giải phẫu / ngoài dải vật lý):
        // xem như frame không có dữ liệu tin cậy - detector không tính là nhắm
        // mắt (tránh false positive).

        // ---- 3. State machine (landmark không hợp lệ -> xem như không có mặt) ----
        const DrowsyState st = s_detector.update(has_face && metrics_ok, m.ear, m.mar, m.pitch, m.yaw, now_ms);
        if (s_state_since_ms == 0) {
            s_state_since_ms = now_ms;
            s_last_state = st;
        } else if (st != s_last_state) {
            const uint64_t previous_state_ms = now_ms - s_state_since_ms;
            ESP_LOGI(TAG,
                     "ALERT_DETECT,state=%s,from=%s,t_ms=%llu,previous_state_ms=%llu,face=%d,ear=%.3f,mar=%.3f,yaw_dev=%.3f",
                     drowsy_state_name(st), drowsy_state_name(s_last_state),
                     (unsigned long long)now_ms,
                     (unsigned long long)previous_state_ms,
                     (has_face && metrics_ok) ? 1 : 0, m.ear, m.mar, s_detector.yaw_dev());
            s_last_state = st;
            s_state_since_ms = now_ms;
        }

        benchmark_log_sample(now_ms, has_face, metrics_ok, s_detector.eyes_closed(),
                             s_detector.yawning(), s_detector.attention_off(), (int)st,
                             m.ear, m.mar, s_detector.yaw_dev());

#if CONFIG_DROWSY_WEB_ENABLE
        if (st != s_last_state) {
            web_log_line("Trạng thái: %d -> %d", (int)s_last_state, (int)st);
            s_last_state = st;
        }
#endif

        // ---- 4. Gửi lệnh cảnh báo (0 timeout: alarm task giữ trạng thái mới nhất) ----
        // Eye/yawn flags are immediate actuator evidence. The detector state
        // remains hysteretic for stable UI/fatigue reporting, but the alarm
        // must not wait for the long-term drowsiness state to change.
        DrowsyState alarm_state = DrowsyState::AWAKE;
        const bool direct_eye_alert = s_detector.eyes_closed();
        const bool direct_yawn_alert = s_detector.yawning();
        const bool direct_attention_alert = s_detector.attention_off();
        if (direct_eye_alert || direct_yawn_alert) {
            alarm_state = st == DrowsyState::MICROSLEEP
                              ? DrowsyState::MICROSLEEP
                              : DrowsyState::DROWSY;
        } else if (direct_attention_alert) {
            alarm_state = DrowsyState::DISTRACTED;
        }
        static DrowsyState s_last_alarm_state = DrowsyState::AWAKE;
        if (alarm_state != s_last_alarm_state) {
            ESP_LOGI(TAG, "ALARM_TRIGGER,detector_state=%d,alarm_state=%d,eye=%d,yawn=%d,attention=%d",
                     (int)st, (int)alarm_state, direct_eye_alert ? 1 : 0,
                     direct_yawn_alert ? 1 : 0, direct_attention_alert ? 1 : 0);
            s_last_alarm_state = alarm_state;
        }
        alarm_event_t ev = {alarm_state};
        xQueueSend(s_alarm_queue, &ev, 0);

#if CONFIG_DROWSY_WEB_ENABLE
        // ---- 4b. Web: cập nhật metrics + vẽ overlay + gửi frame cho web task ----
        // fix M2: ghi metrics dưới spinlock (httpd/report đọc ở Core 0)
        web_metrics_lock();
        s_web_metrics.state = (int)alarm_state;
        s_web_metrics.ear = m.ear;
        s_web_metrics.mar = m.mar;
        s_web_metrics.pitch = m.pitch;
        s_web_metrics.pitch_dev = s_detector.pitch_dev();
        s_web_metrics.yaw_dev = s_detector.yaw_dev();
        s_web_metrics.yaw_baseline = s_detector.yaw_baseline();
        s_web_metrics.perclos = s_detector.perclos();
        s_web_metrics.blink_rate = s_detector.blink_rate();
        s_web_metrics.yawn_rate = s_detector.yawn_rate();
        s_web_metrics.fatigue = s_detector.fatigue_score();
        s_web_metrics.face = has_face ? 1 : 0;
        s_web_metrics.metrics_ok = metrics_ok ? 1 : 0;
        s_web_metrics.eyes_closed = s_detector.eyes_closed() ? 1 : 0;
        s_web_metrics.yawn_active = s_detector.yawning() ? 1 : 0;
        s_web_metrics.attention_off = s_detector.attention_off() ? 1 : 0;
        s_web_metrics.attention_off_ms = s_detector.attention_off_ms();
        web_metrics_unlock();
        web_cache_overlay(has_face ? face_box.data() : nullptr,
                          s_landmarks_valid ? s_landmarks_cache : nullptr,
                          PFLD_NUM_POINTS, metrics_ok && s_landmarks_valid);
        if (has_face) {
            web_draw_overlay(fb, face_box.data(),
                             s_landmarks_valid ? s_landmarks_cache : nullptr,
                             metrics_ok && s_landmarks_valid, &s_web_metrics);
        }
        // QUAN TRỌNG: nếu web queue đầy (chưa có client) -> PHẢI fb_return ngay
        // nếu không sẽ rò frame buffer -> camera hết buffer -> hệ thống treo!
        if (xQueueSend(s_web_frame_queue, &fb, 0) != pdTRUE) {
            // fix lag: queue đầy nghĩa là httpd chưa kịp tiêu thụ -> drop frame
            // CŨ nhất (không phải frame mới) để stream luôn hiển thị frame mới
            // nhất, giảm độ trễ hiển thị (trước đây drop frame mới -> stream trễ).
            camera_fb_t *stale = NULL;
            if (xQueueReceive(s_web_frame_queue, &stale, 0) == pdTRUE && stale != NULL) {
                esp_camera_fb_return(stale);
            }
            // thử lại gửi frame mới (queue giờ có 1 chỗ trống)
            if (xQueueSend(s_web_frame_queue, &fb, 0) != pdTRUE) {
                esp_camera_fb_return(fb);
            }
        }
#else
        esp_camera_fb_return(fb);
#endif
        loop_ms_last = (int)((esp_timer_get_time() - t_loop) / 1000); // tổng thời gian 1 frame
        frames++;

        // ---- 5. Log định kỳ: FPS + số liệu realtime ----
        const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - t0_us) / 1000);
        if (elapsed_ms >= log_interval_ms) {
            const float fps = frames * 1000.0f / (float)elapsed_ms;
#if CONFIG_DROWSY_WEB_ENABLE
            web_metrics_lock(); // fix M2
            s_web_metrics.ai_fps = fps;
            web_metrics_unlock();
#endif
            // face/metrics_ok/roll + face_ms/pfld_ms dùng để DEBUG (face=0, FPS thấp)
            ESP_LOGI(TAG,
                     "FPS=%.1f state=%d face=%d metrics=%d roll=%.2f "
                     "ear=%.2f mar=%.2f pitch=%.2f dev=%.2f perclos=%.0f%% "
                     "blink/min=%.1f yawn/min=%.1f fatigue=%.2f "
                     "face_ms=%d pfld_ms=%d wait_ms=%d loop_ms=%d px_r=%d px_b=%d "
                     "avg_r=%d avg_b=%d msr=%d mnp=%d",
                     fps, (int)st, has_face ? 1 : 0, metrics_ok ? 1 : 0, m.roll,
                     m.ear, m.mar, m.pitch, s_detector.pitch_dev(),
                     s_detector.perclos() * 100.0f, s_detector.blink_rate(),
                     s_detector.yawn_rate(), s_detector.fatigue_score(),
                     face_ms_last, pfld_ms_last, wait_ms_last, loop_ms_last,
                     px_r_last, px_b_last, avg_r_last, avg_b_last,
                     msr_cnt_last, mnp_cnt_last);
            frames = 0;
            t0_us = esp_timer_get_time();
        }
        // fix WDT: với fb_count=4, ai_task chạy LIÊN TỤC 100% CPU1 (s_frame_queue
        // không bao giờ rỗng -> xQueueReceive không block) -> IDLE1 (prio 0) bị
        // starve -> Task WDT trigger. Nhường CPU để IDLE1 chạy và reset watchdog.
        // Nhường CPU 1 tick (10ms) để IDLE1 reset Task WDT và WiFi/httpd xử lý mượt hơn
        vTaskDelay(1);
    }
}

#endif // CONFIG_DROWSY_AI_ON_DEVICE

#if !CONFIG_DROWSY_AI_ON_DEVICE
// ----------------------------------------------------------------------------
// camera_stream_task - khi APP là nguồn (edge AI OFF):
// Đọc frame từ s_frame_queue, đẩy trực tiếp vào s_web_frame_queue để httpd /stream.
// KHÔNG detect/state -> KHÔNG sinh trạng thái edge -> không xung đột 2 luồng state.
// ----------------------------------------------------------------------------
static void camera_stream_task(void *arg)
{
    while (true) {
        camera_fb_t *fb = NULL;
        if (xQueueReceive(s_frame_queue, &fb, portMAX_DELAY) != pdTRUE || fb == NULL) {
            continue;
        }
#if CONFIG_DROWSY_WEB_ENABLE
        // đẩy frame vào web queue cho /stream (dùng chung fb letterpath như web_drain)
        if (xQueueSend(s_web_frame_queue, &fb, 0) != pdTRUE) {
            camera_fb_t *stale = NULL;
            if (xQueueReceive(s_web_frame_queue, &stale, 0) == pdTRUE && stale != NULL) {
                esp_camera_fb_return(stale);
            }
            if (xQueueSend(s_web_frame_queue, &fb, 0) != pdTRUE) {
                esp_camera_fb_return(fb);
            }
        }
#else
        esp_camera_fb_return(fb);
#endif
        // Nhường CPU để IDLE1 reset hệ thống watchdog (WDT)
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif // !CONFIG_DROWSY_AI_ON_DEVICE

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Driver Drowsiness Detection - ESP32-S3-N16R8 (ESP-WHO + ESP-DL v3 + PFLD)");

    // ---- 0. NVS + cấu hình thiết bị (admin panel) - NẠP TRƯỚC MỌI THỨ ----
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    config_store_load(&s_device_config);

    // ---- 1. Nạp model (face detect + PFLD) từ RODATA nhúng ----
    // Chỉ khi ESP32 là nguồn (edge AI on). Nếu offload (app là nguồn) ->
    // không cần face/PFLD -> tiết kiệm RAM/CPU.
#if CONFIG_DROWSY_AI_ON_DEVICE
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is unavailable. This N16R8 firmware requires 8 MB Octal PSRAM.");
        return;
    }
    if (!s_face.init()) {
        ESP_LOGE(TAG, "Face detection model init FAILED!");
        return;
    }
    if (!s_pfld.init()) {
        ESP_LOGE(TAG, "PFLD landmark model init FAILED!");
        return;
    }
#endif

    // ---- 2. Alarm (buzzer + LED) ----
    s_alarm_queue = s_alarm.init();

    // ---- 3. Camera: RGB565, 240x240 (Kconfig), PSRAM ----
    // AI only needs the newest frame; a one-item queue prevents it from
    // accumulating old work while the camera keeps the web stream moving.
    s_frame_queue = xQueueCreate(1, sizeof(camera_fb_t *));
#if CONFIG_DROWSY_WEB_ENABLE
    s_web_frame_queue = xQueueCreate(3, sizeof(camera_fb_t *));
    if (s_web_frame_queue == NULL) {
        ESP_LOGE(TAG, "Web frame queue allocation failed");
        return;
    }
#endif
    const pixformat_t pix = PIXFORMAT_RGB565;
    const framesize_t fsize =
#if CONFIG_DROWSY_FRAME_SIZE_QVGA
        FRAMESIZE_QVGA;
#else
        FRAMESIZE_240X240;
#endif
    // fix lag stream: fb_count=4 (trước 2). Khi stream bật, httpd giữ frame để
    // encode+gửi TCP chậm -> nếu chỉ 2 buffer, camera hết buffer trống ->
    // fb_get() block -> s_frame_queue trống -> AI chờ (wait_ms ~1900ms, FPS tụt).
    // 4 buffer: camera luôn có buffer trống, AI không bao giờ chờ frame.
    // Tốn thêm 2x240x240x2 = 230KB PSRAM (N16R8 có 8MB - dư dả).
    if (!register_camera(pix, fsize, 6, s_frame_queue,
#if CONFIG_DROWSY_WEB_ENABLE
                         s_web_frame_queue
#else
                         NULL
#endif
                         )) {
        ESP_LOGE(TAG, "Camera init FAILED - kiểm tra pin trong menuconfig!");
        return;
    }

    // ---- 4. Áp ngưỡng NVS vào detector + web server (Core 1) ----
    apply_config_to_detector();
#if CONFIG_DROWSY_WEB_ENABLE
    web_server_init(s_web_frame_queue, &s_web_metrics, &s_detector, &s_device_config);
    // fix offload: app gửi POST /api/alarm -> ESP32 bật buzzer/LED
    web_server_set_alarm_queue(s_alarm_queue);
#endif
    // ---- 4b. Báo trạng thái lên Admin Server (đa thiết bị, STA mode) ----
#if CONFIG_DROWSY_REPORT_ENABLE && CONFIG_DROWSY_WIFI_MODE_STA
    report_client_start(&s_device_config, &s_web_metrics, &s_detector);
#endif
    // NGUỒN STATE: nếu ESP32 self (edge+=ai) -> chạy ai_task (tự nhận diện).
    // Nếu app là nguồn (DROWSY_AI_ON_DEVICE=n) -> chỉ cần stream frame cho /stream,
    // không detect/state -> camera_stream_task.
#if CONFIG_DROWSY_AI_ON_DEVICE
    xTaskCreatePinnedToCore(ai_task, "ai_task", 10 * 1024, NULL, 5, NULL, 1);
#else
    xTaskCreatePinnedToCore(camera_stream_task, "stream_task", 4 * 1024, NULL, 5, NULL, 1);
#endif

    // ---- 5. Console runtime: drowsy get/set/stats/reset ----
#if CONFIG_DROWSY_CONSOLE_ENABLE
    register_drowsy_commands(&s_detector, &s_device_config, &s_alarm); // console + alarm test
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "drowsy>";
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // The board exposes the monitor on USB Serial/JTAG. Use that same RX path
    // for interactive commands instead of UART0, whose RX is not on COM7.
    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));
#else
    esp_console_dev_uart_config_t hw_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_cfg, &repl_cfg, &repl));
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
#endif

    ESP_LOGI(TAG, "System ready. Chờ khuôn mặt... (monitor: idf.py monitor)");
}
