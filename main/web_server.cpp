// ============================================================================
// web_server.cpp - Web UI giám sát (WiFi STA + mDNS + httpd + MJPEG stream)
// ----------------------------------------------------------------------------
// Kiến trúc (giống esp-who human_face_detection/web):
//   AI task (Core 1) vẽ overlay -> gửi frame vào xQueueWebFrame
//   - Có client stream: stream_handler tự tiêu thụ queue (encode JPEG + gửi)
//   - Không có client : drain task tiêu thụ + fb_return (pipeline không nghẽn)
//
// Endpoint:
//   GET /        -> trang HTML   (index.html nhúng)
//   GET /stream  -> MJPEG stream (multipart/x-mixed-replace)
//   GET /capture -> 1 ảnh JPEG
//   GET /status  -> JSON metrics
// ============================================================================

// QUAN TRỌNG: phải include sdkconfig.h TRƯỚC #if - nếu không CONFIG_DROWSY_*
// không được định nghĩa tại thời điểm tiền xử lý -> toàn bộ code web bị compile
// thành stub (không AP, không stream, không admin)!
#include "sdkconfig.h"

#if CONFIG_DROWSY_WEB_ENABLE

#include "web_server.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "landmark_pfld.hpp"  // PFLD_NUM_POINTS (68/98) cho overlay
#include "alarm_control.hpp"  // alarm_event_t (POST /api/alarm)

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "mdns.h"
#include "nvs_flash.h"

static const char *TAG = "web_server";

static QueueHandle_t s_frame_queue = NULL;
static QueueHandle_t s_alarm_queue = NULL; // fix offload: app gửi POST /api/alarm
static web_metrics_t *s_metrics = NULL;
static DrowsinessDetector *s_detector = NULL;
static device_config_t *s_config = NULL;
static volatile bool s_stream_active = false;

// fix M2: spinlock bảo vệ struct web_metrics_t (AI task Core 1 ghi,
// httpd handler + report_client Core 0 đọc) chống torn read.
static portMUX_TYPE s_metrics_mux = portMUX_INITIALIZER_UNLOCKED;

void web_metrics_lock(void)
{
    portENTER_CRITICAL(&s_metrics_mux);
}

void web_metrics_unlock(void)
{
    portEXIT_CRITICAL(&s_metrics_mux);
}

// fix offload: app đăng ký queue alarm (từ AlarmControl::init) để POST /api/alarm
void web_server_set_alarm_queue(QueueHandle_t q)
{
    s_alarm_queue = q;
}

// ---- Ring log nhỏ cho /admin/api/log ----
#define LOG_RING_MAX 32
#define LOG_LINE_MAX 96
static char s_log_ring[LOG_RING_MAX][LOG_LINE_MAX];
static int s_log_idx = 0;
static SemaphoreHandle_t s_log_mutex = NULL; // fix A2: chống race AI task (Core 1) vs httpd (Core 0)

void web_log_line(const char *fmt, ...)
{
    if (s_log_mutex == NULL) return;
    va_list ap;
    va_start(ap, fmt);
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        vsnprintf(s_log_ring[s_log_idx], LOG_LINE_MAX, fmt, ap);
        s_log_idx = (s_log_idx + 1) % LOG_RING_MAX;
        xSemaphoreGive(s_log_mutex);
    }
    va_end(ap);
}

// ---- MJPEG stream constants ----
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CT =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

// ============================================================================
// Overlay drawing trên RGB565 (5-6-5) - tự viết, không phụ thuộc fb_gfx
// ============================================================================

// Font 5x7: index = (char - 0x20), hỗ trợ A-Z, 0-9, ' ', '.', ':', '-', '%', '/', '_'
static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x00,0x00,0x00}, // !
    {0x00,0x00,0x00,0x00,0x00}, // "
    {0x00,0x00,0x00,0x00,0x00}, // #
    {0x00,0x00,0x00,0x00,0x00}, // $
    {0x43,0x23,0x08,0x64,0x61}, // %
    {0x00,0x00,0x00,0x00,0x00}, // &
    {0x00,0x00,0x00,0x00,0x00}, // '
    {0x00,0x00,0x00,0x00,0x00}, // (
    {0x00,0x00,0x00,0x00,0x00}, // )
    {0x00,0x00,0x00,0x00,0x00}, // *
    {0x00,0x08,0x08,0x08,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00}, // ,
    {0x00,0x08,0x08,0x08,0x00}, // -
    {0x00,0x00,0x60,0x60,0x00}, // .
    {0x60,0x10,0x08,0x04,0x03}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x00,0x00,0x00,0x00}, // ;
    {0x00,0x00,0x00,0x00,0x00}, // <
    {0x00,0x14,0x14,0x14,0x00}, // =
    {0x00,0x00,0x00,0x00,0x00}, // >
    {0x00,0x00,0x00,0x00,0x00}, // ?
    {0x00,0x00,0x00,0x00,0x00}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x00,0x00,0x00,0x00}, // [
    {0x00,0x00,0x00,0x00,0x00}, // backslash
    {0x00,0x00,0x00,0x00,0x00}, // ]
    {0x00,0x00,0x00,0x00,0x00}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
};

static inline void px(uint16_t *buf, int w, int h, int x, int y, uint16_t color)
{
    if (x >= 0 && x < w && y >= 0 && y < h) {
        buf[y * w + x] = color;
    }
}

static void draw_char(uint16_t *buf, int w, int h, int x, int y, char c, uint16_t color)
{
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A'; // vẽ HOA cho gọn font
    int idx = c - 0x20;
    if (idx < 0 || idx > 0x5F - 0x20) return;
    const uint8_t *g = FONT5X7[idx];
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (g[col] & (1 << row)) {
                px(buf, w, h, x + col, y + row, color);
            }
        }
    }
}

static void draw_text(uint16_t *buf, int w, int h, int x, int y, const char *s, uint16_t color)
{
    while (*s) {
        draw_char(buf, w, h, x, y, *s, color);
        x += 6; // 5px + 1px cách
        ++s;
    }
}

static void draw_rect(uint16_t *buf, int w, int h, int x0, int y0, int x1, int y1, uint16_t color)
{
    for (int x = x0; x <= x1; ++x) { px(buf, w, h, x, y0, color); px(buf, w, h, x, y1, color); }
    for (int y = y0; y <= y1; ++y) { px(buf, w, h, x0, y, color); px(buf, w, h, x1, y, color); }
}

#define RGB565_RED   0xF800
#define RGB565_GREEN 0x07E0
#define RGB565_BLUE  0x001F
#define RGB565_YELLOW 0xFFE0
#define RGB565_WHITE 0xFFFF

void web_draw_overlay(camera_fb_t *fb,
                      const int *face_box,
                      const float *landmarks,
                      bool face_ok,
                      const web_metrics_t *metrics)
{
    if (fb == NULL || fb->format != PIXFORMAT_RGB565 || fb->buf == NULL) {
        return;
    }
    uint16_t *buf = (uint16_t *)fb->buf;
    const int w = fb->width, h = fb->height;

    // ---- Khung mặt (xanh lá) ----
    if (face_box != NULL) {
        draw_rect(buf, w, h, face_box[0], face_box[1], face_box[2], face_box[3], RGB565_GREEN);
    }

    // ---- landmark (màu theo vùng, mapping theo model 68/98) ----
    if (face_ok && landmarks != NULL) {
        for (int i = 0; i < PFLD_NUM_POINTS; ++i) {
            int lx = (int)landmarks[i * 2], ly = (int)landmarks[i * 2 + 1];
            uint16_t color = RGB565_GREEN;
#if PFLD_NUM_POINTS == 68
            if (i >= 36 && i <= 47) color = RGB565_RED;    // mắt (iBUG)
            else if (i >= 27 && i <= 35) color = RGB565_BLUE; // mũi
            else if (i >= 48 && i <= 67) color = RGB565_YELLOW; // miệng
#else
            if (i >= 60 && i <= 75) color = RGB565_RED;    // mắt (WFLW)
            else if (i >= 51 && i <= 59) color = RGB565_BLUE; // mũi
            else if (i >= 76 && i <= 95) color = RGB565_YELLOW; // miệng
#endif
            px(buf, w, h, lx, ly, color);
            px(buf, w, h, lx + 1, ly, color);
            px(buf, w, h, lx, ly + 1, color);
            px(buf, w, h, lx + 1, ly + 1, color);
        }
    }

    // ---- Text metrics (góc trên trái) ----
    if (metrics != NULL) {
        char line1[40], line2[40], line3[40];
        snprintf(line1, sizeof(line1), "EAR %.2f MAR %.2f", metrics->ear, metrics->mar);
        snprintf(line2, sizeof(line2), "ST %d FPS %.1f PER %d%%", metrics->state,
                 metrics->fps, (int)(metrics->perclos * 100.0f));
        snprintf(line3, sizeof(line3), "FAC %d BL %.0f YW %.0f", metrics->face,
                 metrics->blink_rate, metrics->yawn_rate);
        uint16_t c1 = (metrics->state >= 2) ? RGB565_RED : RGB565_WHITE;
        draw_text(buf, w, h, 2, 2, line1, c1);
        draw_text(buf, w, h, 2, 10, line2, c1);
        draw_text(buf, w, h, 2, 18, line3, RGB565_WHITE);
    }
}

// ============================================================================
// HTTP handlers
// ============================================================================

static esp_err_t index_handler(httpd_req_t *req)
{
    extern const uint8_t index_html_start[] asm("_binary_index_html_start");
    extern const uint8_t index_html_end[] asm("_binary_index_html_end");
    const size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[320];
    const char *st[] = {"AWAKE", "PRE_DROWSY", "DROWSY", "MICROSLEEP"};
    // fix M2: copy metrics dưới spinlock (AI task Core 1 ghi) tránh torn read
    web_metrics_t m = {};
    web_metrics_lock();
    if (s_metrics) m = *s_metrics;
    web_metrics_unlock();
    int st_idx = m.state;
    if (st_idx < 0 || st_idx > 3) st_idx = 0;
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"state_id\":%d,\"ear\":%.3f,\"mar\":%.3f,"
             "\"pitch\":%.3f,\"pitch_dev\":%.3f,\"perclos\":%.1f,\"blink_min\":%.1f,"
             "\"yawn_min\":%.1f,\"fatigue\":%.2f,\"fps\":%.1f,\"face\":%d,\"metrics_ok\":%d}",
             st[st_idx], st_idx,
             m.ear, m.mar,
             m.pitch, m.pitch_dev,
             m.perclos * 100.0f,
             m.blink_rate, m.yawn_rate,
             m.fatigue, m.fps, m.face, m.metrics_ok);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, strlen(buf));
}

// POST /api/alarm — app (AI offload) báo state muốn ESP32 bật buzzer/LED
// Body JSON: {"state": 0-3} (DrowsyState). Best-effort.
static esp_err_t alarm_handler(httpd_req_t *req)
{
    char body[32] = {0};
    int state = 0;
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n >= 0) {
        body[n] = 0;
        // parse JSON {"state": N}
        char *p = strchr(body, ':');
        if (p) state = atoi(p + 1);
    }
    if (state < 0) state = 0;
    if (state > 3) state = 3;

    // NGUỒN STATE duy nhất: chỉ khi APP đang là nguồn (edge AI OFF) mới nhận
    // lệnh báo động từ app. Nếu edge AI ON (tự nhận diện), app chỉ hiển thị,
    // buzzer/LED do edge tự đưa ra -> bỏ qua /api/alarm để tránh 2 nguồn đua.
#if !CONFIG_DROWSY_AI_ON_DEVICE
    if (s_alarm_queue != NULL) {
        alarm_event_t ev;
        ev.state = (DrowsyState)state;
        xQueueSend(s_alarm_queue, &ev, 0);
        web_log_line("Alarm từ app: state=%d", state);
    }
#endif

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ============================================================================
// POST /api/ota — cập nhật firmware qua HTTP (app POST file .bin raw body)
// Body: raw binary firmware (Content-Type: application/octet-stream).
// Ghi vào partition OTA trống, set boot partition, restart.
// ============================================================================
#define OTA_BUF_SIZE  (4 * 1024)
#define OTA_MAX_PART  (0x3C0000) // = size partition ota_0/ota_1 (xem partitions.csv)

static esp_err_t ota_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // Lấy partition OTA tiếp theo (ota_1 nếu đang chạy ota_0, và ngược lại).
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        web_log_line("OTA: không có partition update");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: bắt đầu nhận firmware -> %s", update_part->label);

    static const char *OTA_JSON_PREFIX = "{\"ok\":true,\"next_boot\":\"";
    static const char *OTA_JSON_SUFFIX = "\"}";

    // Số byte client thông báo sẽ gửi (headers). Nếu không có/hợp lệ -> lỗi.
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > OTA_MAX_PART) {
        ESP_LOGW(TAG, "OTA: Content-Length không hợp lệ (%d, tối đa %d)",
                 content_len, OTA_MAX_PART);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin fail: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(OTA_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "OTA: không cấp được buffer");
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int total = 0;
    int recv_len = 0;
    while (total < content_len) {
        recv_len = httpd_req_recv(req, buf, OTA_BUF_SIZE);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; // client chậm giữa 2 chunk — thử lại
        }
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "OTA: lỗi recv (len=%d)", recv_len);
            break;
        }
        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: esp_ota_write fail: %s", esp_err_to_name(err));
            break;
        }
        total += recv_len;
    }
    free(buf);

    if (recv_len <= 0 || total != content_len) {
        ESP_LOGE(TAG, "OTA: nhận không đủ body (%d/%d)", total, content_len);
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end fail: %s (image không hợp lệ?)", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Kiểm tra image hợp lệ rồi mới chọn làm boot partition.
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_set_boot_partition fail: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    web_log_line("OTA hoàn tất (%d bytes) -> %s", total, update_part->label);
    ESP_LOGI(TAG, "OTA: thành công %d bytes, boot sang %s, restart...",
             total, update_part->label);

    // Báo client kết quả (next_boot = label partition thực) trước khi restart.
    httpd_resp_sendstr(req, OTA_JSON_PREFIX);
    httpd_resp_sendstr(req, update_part->label);
    httpd_resp_sendstr(req, OTA_JSON_SUFFIX);

    vTaskDelay(pdMS_TO_TICKS(500)); // cho phép response được flush
    esp_restart();
    return ESP_OK; // không tới được (esp_restart không trả về)
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    // fix B4: capture lấy frame TRỰC TIẾP từ camera (không đụng s_frame_queue
    // chung với stream - tránh tranh frame khi cả 2 cùng chạy).
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    esp_err_t res = ESP_FAIL;
    if (frame2jpg(fb, 80, &jpg, &jpg_len)) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        res = httpd_resp_send(req, (const char *)jpg, jpg_len);
    }
    if (jpg) free(jpg);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    char part_buf[128];

    res = httpd_resp_set_type(req, STREAM_CT);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

    s_stream_active = true;
    int skip = 0;
    // fix lag: chất lượng JPEG + tỉ lệ skip lấy từ Kconfig (dễ tinh chỉnh)
    const int jpeg_q = CONFIG_DROWSY_WEB_JPEG_QUALITY;
    const int stream_skip = CONFIG_DROWSY_WEB_STREAM_SKIP;
    while (res == ESP_OK) {
        if (xQueueReceive(s_frame_queue, &fb, pdMS_TO_TICKS(2000)) != pdTRUE || fb == NULL) {
            res = ESP_FAIL; // hết frame (client vẫn mở) -> thoát, client refresh lại
            break;
        }
        // fix lag: gửi 1/N frame (mặc định 1/3) - giảm tải JPEG encode + bandwidth.
        // Trước đây skip 1/2 cứng; để Kconfig chỉnh được khi vẫn lag.
        if ((skip++ % stream_skip) != 0) {
            esp_camera_fb_return(fb);
            continue;
        }
        jpg = NULL;
        jpg_len = 0;
        if (!frame2jpg(fb, jpeg_q, &jpg, &jpg_len)) {
            esp_camera_fb_return(fb);
            res = ESP_FAIL;
            break;
        }
        esp_camera_fb_return(fb);

        if (res == ESP_OK) res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpg_len,
                                   (int)0, (int)0);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg, jpg_len);
        free(jpg);
    }
    httpd_resp_send_chunk(req, NULL, 0); // kết thúc multipart
    s_stream_active = false;
    return res;
}

// ---- Drain task: khi KHÔNG có client stream, tiêu thụ + fb_return ----
static void web_drain_task(void *arg)
{
    camera_fb_t *fb = NULL;
    while (true) {
        if (!s_stream_active &&
            xQueueReceive(s_frame_queue, &fb, pdMS_TO_TICKS(100)) == pdTRUE && fb != NULL) {
            esp_camera_fb_return(fb);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ============================================================================
// ADMIN PANEL - quản lý cấu hình thiết bị
// ============================================================================

// Kiểm tra mật khẩu admin (query param "pass")
static bool admin_auth_ok(httpd_req_t *req)
{
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    char pass[64] = {0};
    if (httpd_query_key_value(buf, "pass", pass, sizeof(pass)) != ESP_OK) {
        return false;
    }
    return strcmp(pass, CONFIG_DROWSY_ADMIN_PASS) == 0;
}

static void admin_send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Sai mật khẩu admin", HTTPD_RESP_USE_STRLEN);
}

// GET /admin -> trang quản trị
static esp_err_t admin_page_handler(httpd_req_t *req)
{
    extern const uint8_t admin_html_start[] asm("_binary_admin_html_start");
    extern const uint8_t admin_html_end[] asm("_binary_admin_html_end");
    const size_t len = admin_html_end - admin_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "identity");
    return httpd_resp_send(req, (const char *)admin_html_start, len);
}

// GET /admin/api/log -> vài dòng log gần nhất
static esp_err_t admin_log_handler(httpd_req_t *req)
{
    if (!admin_auth_ok(req)) {
        admin_send_401(req);
        return ESP_OK;
    }
    char out[LOG_RING_MAX * LOG_LINE_MAX + 32];
    size_t n = 0;
    // fix A3: mỗi snprintf trả về số cần viết (có thể > còn lại) -> chỉ cộng
    // số THỰC SỰ đã viết (min(len, còn lại)) để n không vượt buffer -> OOB
    size_t cap = sizeof(out);
    n += snprintf(out + n, cap - n, "[\"");
    if (n > cap) n = cap;
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) { // fix A2
        for (int i = 0; i < LOG_RING_MAX; ++i) {
            int idx = (s_log_idx + i) % LOG_RING_MAX;
            if (s_log_ring[idx][0] == 0) continue;
            int w = snprintf(out + n, cap - n, "%s\\n\",\"", s_log_ring[idx]);
            if (w < 0) break;
            n += (size_t)w;
            if (n >= cap) { n = cap - 1; break; } // clamp - fix A3
        }
        xSemaphoreGive(s_log_mutex);
    }
    int w = snprintf(out + n, cap - n, "\"]");
    if (w > 0) { n += (size_t)w; if (n >= cap) n = cap - 1; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, out, n);
}

// GET /admin/api/config?pass=xxx          -> JSON cấu hình hiện tại
// GET /admin/api/config?pass=xxx&k=v&...  -> cập nhật + lưu NVS + áp dụng runtime
static esp_err_t admin_config_handler(httpd_req_t *req)
{
    if (!admin_auth_ok(req)) {
        admin_send_401(req);
        return ESP_OK;
    }
    char qbuf[512];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK) {
        admin_send_401(req);
        return ESP_OK;
    }

    // Phân biệt "xem cấu hình" vs "cập nhật": nếu có BẤT KỲ tham số cập nhật
    // nào trong URL thì vào nhánh cập nhật; chỉ khi không có tham số nào
    // (ngoài pass) mới trả JSON cấu hình hiện tại.
    // (Trước đây chỉ kiểm tra ear_blink_th -> lệnh WiFi bị nhầm thành "xem".)
    const char *UPDATE_KEYS[] = {
        "ear_blink_th", "ear_drowsy_th", "mar_th", "perclos_th",
        "pitch_dev_th", "microsleep_ms", "window_ms", "blink_rate_high",
        "wifi_mode", "wifi_ssid", "wifi_pass",
        "device_name", "driver_name", "location",
    };
    // fix: KHÔNG dùng httpd_query_key_value(qbuf,key,NULL,0) để dò key — hàm này
    // trả ESP_ERR_INVALID_ARG khi val==NULL -> has_update LUÔN false -> firmware
    // luôn trả JSON cấu hình thay vì áp lệnh (bug "thiết bị trả cấu hình").
    // Thay bằng quét text trên query string: có "<key>=" đúng boundary thì update.
    bool has_update = false;
    for (size_t i = 0; i < sizeof(UPDATE_KEYS) / sizeof(UPDATE_KEYS[0]); ++i) {
        char needle[48];
        snprintf(needle, sizeof(needle), "%s=", UPDATE_KEYS[i]);
        if (strstr(qbuf, needle) != NULL) {
            has_update = true;
            break;
        }
    }
    if (!has_update) {
        char js[640];
        snprintf(js, sizeof(js),
                 "{\"device_name\":\"%s\",\"device_id\":\"%s\","
                 "\"driver_name\":\"%s\",\"location\":\"%s\","
                 "\"wifi_ssid\":\"%s\",\"wifi_mode\":%d,"
                 "\"ear_blink_th\":%d,\"ear_drowsy_th\":%d,\"mar_th\":%d,"
                 "\"perclos_th\":%d,\"pitch_dev_th\":%d,\"microsleep_ms\":%d,"
                 "\"window_ms\":%d,\"blink_rate_high\":%d}",
                 s_config->device_name, s_config->device_id,
                 s_config->driver_name, s_config->location,
                 s_config->wifi_ssid, (int)s_config->wifi_mode,
                 (int)s_config->ear_blink_th_x1000, (int)s_config->ear_drowsy_th_x1000,
                 (int)s_config->mar_th_x1000, (int)s_config->perclos_th_x1000,
                 (int)s_config->pitch_dev_th_x1000, (int)s_config->microsleep_ms,
                 (int)s_config->window_ms, (int)s_config->blink_rate_high);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, js, strlen(js));
    }

    // Có tham số -> cập nhật từng trường (đơn vị x1000 cho tỷ lệ)
    char v[64];
#define GET_I(field)                                 \
    do {                                              \
        if (httpd_query_key_value(qbuf, #field, v, sizeof(v)) == ESP_OK) { \
            s_config->field = atoi(v);                \
        }                                             \
    } while (0)
    GET_I(ear_blink_th_x1000);
    GET_I(ear_drowsy_th_x1000);
    GET_I(mar_th_x1000);
    GET_I(perclos_th_x1000);
    GET_I(pitch_dev_th_x1000);
    GET_I(microsleep_ms);
    GET_I(window_ms);
    GET_I(blink_rate_high);
    int wifi_mode_changed = 0;
    // Đổi wifi_mode -> cần reboot để áp (AP<->STA) - lưu lại giá trị cũ trước khi set
    if (httpd_query_key_value(qbuf, "wifi_mode", v, sizeof(v)) == ESP_OK) {
        wifi_mode_changed = (atoi(v) != (int)s_config->wifi_mode);
        s_config->wifi_mode = atoi(v);
    }
    // %.Ns: giới hạn độ dài copy = cỡ đích - 1 (tránh -Werror=format-truncation)
    if (httpd_query_key_value(qbuf, "device_name", v, sizeof(v)) == ESP_OK) {
        snprintf(s_config->device_name, sizeof(s_config->device_name), "%.*s",
                 (int)sizeof(s_config->device_name) - 1, v);
    }
    if (httpd_query_key_value(qbuf, "driver_name", v, sizeof(v)) == ESP_OK) {
        snprintf(s_config->driver_name, sizeof(s_config->driver_name), "%.*s",
                 (int)sizeof(s_config->driver_name) - 1, v);
    }
    if (httpd_query_key_value(qbuf, "location", v, sizeof(v)) == ESP_OK) {
        snprintf(s_config->location, sizeof(s_config->location), "%.*s",
                 (int)sizeof(s_config->location) - 1, v);
    }
    // wifi_ssid / wifi_pass (STA)
    if (httpd_query_key_value(qbuf, "wifi_ssid", v, sizeof(v)) == ESP_OK) {
        snprintf(s_config->wifi_ssid, sizeof(s_config->wifi_ssid), "%.*s",
                 (int)sizeof(s_config->wifi_ssid) - 1, v);
    }
    if (httpd_query_key_value(qbuf, "wifi_pass", v, sizeof(v)) == ESP_OK) {
        snprintf(s_config->wifi_pass, sizeof(s_config->wifi_pass), "%.*s",
                 (int)sizeof(s_config->wifi_pass) - 1, v);
    }
#undef GET_I

    config_store_save(s_config);

    // Áp dụng runtime vào detector
    if (s_detector != NULL) {
        DrowsyParams p = s_detector->params();
        p.ear_blink_th = s_config->ear_blink_th_x1000 / 1000.0f;
        p.ear_drowsy_th = s_config->ear_drowsy_th_x1000 / 1000.0f;
        p.mar_th = s_config->mar_th_x1000 / 1000.0f;
        p.perclos_th = s_config->perclos_th_x1000 / 1000.0f;
        p.pitch_dev_th = s_config->pitch_dev_th_x1000 / 1000.0f;
        p.microsleep_ms = (uint32_t)s_config->microsleep_ms;
        p.window_ms = (uint32_t)s_config->window_ms;
        p.blink_rate_high = (uint32_t)s_config->blink_rate_high;
        s_detector->apply_params(p);
    }
    web_log_line("Admin: cấu hình đã lưu + áp dụng");
    // Nếu đổi wifi_mode -> cần REBOOT để áp AP/STA mới (trả về chuỗi đặc biệt
    // để app/trang web biết thiết bị sắp khởi động lại)
    if (wifi_mode_changed) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_send(req, "OK_WIFI_REBOOT", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart(); // áp AP/STA mới
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// GET /admin/api/reboot?pass=xxx -> khởi động lại (áp WiFi/AP mới)
static esp_err_t admin_reboot_handler(httpd_req_t *req)
{
    if (!admin_auth_ok(req)) {
        admin_send_401(req);
        return ESP_OK;
    }
    web_log_line("Admin: reboot");
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

// ============================================================================
// WiFi STA + mDNS
// ============================================================================

static int s_sta_retries = 0; // fix C1: đếm số lần thử lại (chống loop vô hạn)

// forward decl: STA fail -> fallback AP (định nghĩa phía sau)
static void wifi_ap_init(void);

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_sta_retries = 0;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_sta_retries <= 5) { // fix C1: thử lại tối đa 5 lần, sau đó bỏ
            ESP_LOGW(TAG, "WiFi mất kết nối - thử lại (%d/5)...", s_sta_retries);
            esp_wifi_connect();
        } else {
            // fix STA fallback: sau 5 lần fail, tự quay về AP mode để app luôn
            // truy cập được (192.168.4.1) thay vì 'chết cứng' không AP/STA.
            ESP_LOGE(TAG, "STA không kết nối được sau 5 lần - TỰ quay lại AP mode.");
            if (s_config != NULL) {
                s_config->wifi_mode = 0; // AP
                config_store_save(s_config);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart(); // reboot để chạy wifi_ap_init từ đầu
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_retries = 0;
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected: " IPSTR " (http://drowsy.local hoặc http://" IPSTR ")",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip));
    }
}

// ============================================================================
// WIFI (runtime): đọc cấu hình từ NVS (s_config - app gửi qua /admin/api/config)
// với FALLBACK Kconfig. Đây là chìa khoá để app đổi AP<->STA mà không cần rebuild.
// ============================================================================

static void wifi_sta_init(void)
{
    if (s_config == NULL) return;
    // Ưu tiên NVS (app đã gửi); nếu trống -> fallback Kconfig (chỉ khi STA đã bật
    // trong menuconfig - option CONFIG_DROWSY_WIFI_SSID depends on STA nên AP mode
    // KHÔNG có macro này -> phải #if defined)
    const char *ssid = s_config->wifi_ssid[0] ? s_config->wifi_ssid
#if defined(CONFIG_DROWSY_WIFI_SSID)
                                              : CONFIG_DROWSY_WIFI_SSID;
    const char *pass = s_config->wifi_pass[0] ? s_config->wifi_pass : CONFIG_DROWSY_WIFI_PASS;
#else
                                              : "";
    const char *pass = s_config->wifi_pass;
#endif
    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "Chưa có WiFi SSID (STA). Dùng app: Quét WiFi -> gửi, hoặc menuconfig.");
        return;
    }
    ESP_LOGI(TAG, "Connecting WiFi STA: SSID=%s", ssid);
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {};
    // %.Ns giới hạn copy = cỡ đích - 1 (tránh -Werror=format-truncation)
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%.*s",
             (int)sizeof(wifi_config.sta.ssid) - 1, ssid);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%.*s",
             (int)sizeof(wifi_config.sta.password) - 1, pass);
    // fix STA: không ép cứng WPA2_PSK => cho phép WPA2/WPA3 transition/WPA3-only.
    // (Mặc định WIFI_AUTH_OPEN để driver tự đàm phán theo router.)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

static void wifi_ap_init(void)
{
    if (s_config == NULL) return;
    // fix: AP mode LUÔN dùng đúng SSID/pass AP — không đọc từ s_config->wifi_ssid
    // (thứ đó có thể còn dữ liệu router từ lần STA trước trong NVS, khiến AP phát
    // SSID "Bac Luan" thay vì "Drowsy_AP" -> không kết nối được như trước).
    // AP SSID/pass đến từ Kconfig (fallback) — sử dụng cho AP mode.
    const char *ssid =
#if defined(CONFIG_DROWSY_AP_SSID)
        CONFIG_DROWSY_AP_SSID;
    const char *pass = CONFIG_DROWSY_AP_PASS;
#else
        "Drowsy_AP";
    const char *pass = ""; // mạng mở (WIFI_AUTH_OPEN)
#endif
    ESP_LOGI(TAG, "Starting AP: SSID=%s", ssid);
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {};
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%.*s",
             (int)sizeof(wifi_config.ap.ssid) - 1, ssid);
    snprintf((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), "%.*s",
             (int)sizeof(wifi_config.ap.password) - 1, pass);
    wifi_config.ap.ssid_len = (uint8_t)strlen(ssid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = (strlen(pass) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) { ESP_LOGE(TAG, "AP set_mode fail: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "AP set_config fail: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "AP wifi_start fail: %s", esp_err_to_name(err)); return; }

    // ---- Tăng throughput AP (fix lag stream MJPEG) ----
    // 1) TẮT power save: AP mặc định bật WIFI_PS_MIN_MODEM -> modem ngủ giữa
    //    các gói -> latency/throughput rất tệ (nguyên nhân chính của
    //    'httpd_sock_err: error in send : 104' khi client timeout).
    esp_wifi_set_ps(WIFI_PS_NONE);
    // 2) HT40: gấp đôi băng thông so với HT20 mặc định.
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
    // 3) Bảo đảm bật 802.11n (mặc định chỉ 11b/g nếu không set).
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

    ESP_LOGI(TAG, "AP ready: kết nối WiFi '%s' rồi mở http://192.168.4.1", ssid);
}

static void mdns_init_custom(void); // forward decl

static void wifi_mode_init(void)
{
    if (s_config == NULL) return;
    // Đọc wifi_mode từ NVS (app đổi runtime) - fallback Kconfig khi chưa có NVS
    int mode = s_config->wifi_mode;
#if !defined(CONFIG_DROWSY_WIFI_MODE_AP) && !defined(CONFIG_DROWSY_WIFI_MODE_STA)
    if (mode == 0) mode = 2; // sdkconfig cũ -> tắt
#endif
    switch (mode) {
    case 0: wifi_ap_init(); break;
    case 1: wifi_sta_init(); break;
    default: ESP_LOGW(TAG, "WiFi TẮT (wifi_mode=%d). Bật qua app/menuconfig.", mode); break;
    }
    mdns_init_custom();
}

static void mdns_init_custom(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed: %d", err);
        return;
    }
    mdns_hostname_set("drowsy");
    mdns_instance_name_set("Driver Drowsiness Monitor");
}

// ============================================================================
// Khởi tạo
// ============================================================================

void web_server_init(QueueHandle_t frame_i,
                     web_metrics_t *metrics,
                     DrowsinessDetector *detector,
                     device_config_t *config)
{
    s_frame_queue = frame_i;
    s_metrics = metrics;
    s_detector = detector;
    s_config = config;
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex(); // fix A2
    }

    wifi_mode_init();

    // ---- HTTP server (port 80): TẤT CẢ endpoint, kể cả /stream ----
    // LƯU Ý (fix camera không stream): trang web dùng <img src="/stream"> (URL
    // tương đối -> port 80), app Android cũng gọi http://<ip>/stream (port 80).
    // Trước đây /stream nằm ở server port 81 riêng -> 404 -> không hiện ảnh!
    // -> gộp /stream vào server port 80. esp_http_server chạy đa luồng
    // (max_open_sockets mặc định 7) nên stream dài không chặn /status /admin.
    httpd_config_t httpd_conf = HTTPD_DEFAULT_CONFIG();
    httpd_conf.max_uri_handlers = 11; // 8 echo + /api/alarm + /api/ota mới
    httpd_conf.server_port = CONFIG_DROWSY_WEB_HTTP_PORT;
    // Pin httpd sang CORE 0: JPEG encode + send stream rất tốn CPU, nếu chạy
    // chung Core 1 với AI pipeline sẽ kéo FPS xuống (9.9 -> 2.5 như đã thấy).
    httpd_conf.core_id = 0;
    // QUAN TRỌNG (fix reboot khi mở stream): stack mặc định 4096 quá nhỏ cho
    // stream_handler + frame2jpg (encode JPEG tốn stack) -> tràn stack -> crash
    // -> esp_restart. Nâng lên 8KB. Ưu tiên thấp (prio 2) để không cướp AI/camera.
    httpd_conf.stack_size = 8192;
    httpd_conf.task_priority = 2; // IDF 5.3: tên trường là task_priority
    // fix lag: client chậm (mạng AP kém) không được block handler lâu -> nếu
    // send/recv treo quá 3s thì httpd coi như lỗi, thoát handler, giải phóng
    // Core 0 (trước đây timeout mặc định rất dài -> handler nghẽn -> app
    // timeout đóng -> 'error in send : 104' -> loop giật).
    httpd_conf.send_wait_timeout = 3;    // giây (field đúng: send_wait_timeout)
    httpd_conf.recv_wait_timeout = 3;    // giây

    httpd_uri_t idx = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_uri_t stt = {.uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    httpd_uri_t cap = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
    httpd_uri_t adm = {.uri = "/admin", .method = HTTP_GET, .handler = admin_page_handler, .user_ctx = NULL};
    httpd_uri_t acf = {.uri = "/admin/api/config", .method = HTTP_GET, .handler = admin_config_handler, .user_ctx = NULL};
    httpd_uri_t arb = {.uri = "/admin/api/reboot", .method = HTTP_GET, .handler = admin_reboot_handler, .user_ctx = NULL};
    httpd_uri_t alg = {.uri = "/admin/api/log", .method = HTTP_GET, .handler = admin_log_handler, .user_ctx = NULL};
    httpd_uri_t str = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};
    httpd_uri_t arm = {.uri = "/api/alarm", .method = HTTP_POST, .handler = alarm_handler, .user_ctx = NULL};
    httpd_uri_t ota = {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler, .user_ctx = NULL};

    httpd_handle_t httpd = NULL;
    if (httpd_start(&httpd, &httpd_conf) == ESP_OK) {
        httpd_register_uri_handler(httpd, &idx);
        httpd_register_uri_handler(httpd, &stt);
        httpd_register_uri_handler(httpd, &cap);
        httpd_register_uri_handler(httpd, &adm);
        httpd_register_uri_handler(httpd, &acf);
        httpd_register_uri_handler(httpd, &arb);
        httpd_register_uri_handler(httpd, &alg);
        httpd_register_uri_handler(httpd, &str);
        httpd_register_uri_handler(httpd, &arm);
        httpd_register_uri_handler(httpd, &ota);
        ESP_LOGI(TAG, "HTTP server on port %d (stream + admin cùng port)", httpd_conf.server_port);
    }

    xTaskCreatePinnedToCore(web_drain_task, "web_drain", 3 * 1024, NULL, 4, NULL, 1);

    web_log_line("Web server khởi động");
}

#else // CONFIG_DROWSY_WEB_ENABLE == n

#include "web_server.hpp"

void web_server_init(QueueHandle_t frame_i,
                     web_metrics_t *metrics,
                     DrowsinessDetector *detector,
                     device_config_t *config)
{
    (void)frame_i; (void)metrics; (void)detector; (void)config;
}
void web_log_line(const char *fmt, ...) { (void)fmt; }
void web_metrics_lock(void) {}
void web_metrics_unlock(void) {}
// fix offload: stub cho trường hợp tắt WEB (không lỗi link)
void web_server_set_alarm_queue(QueueHandle_t q) { (void)q; }
void web_draw_overlay(camera_fb_t *fb, const int *face_box, const float *landmarks,
                      bool face_ok, const web_metrics_t *metrics)
{
    (void)fb; (void)face_box; (void)landmarks; (void)face_ok; (void)metrics;
}

#endif
