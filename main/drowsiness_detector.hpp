#pragma once

// ============================================================================
// drowsiness_detector.hpp - Bộ não logic: EAR/MAR/pitch + State machine
// ----------------------------------------------------------------------------
// Đầu vào mỗi frame:  có mặt không, EAR, MAR, pitch (từ PFLD 98 điểm)
// Đầu ra:             trạng thái AWAKE / PRE_DROWSY / DROWSY / MICROSLEEP
//
// Các metric:
//  - EAR    (Eye Aspect Ratio, công thức Soukupová & Čech 2016)
//  - MAR    (Mouth Aspect Ratio - tỷ lệ khung bao miệng trong)
//  - PERCLOS (% thời gian mắt nhắm trong cửa sổ trượt - chuẩn y khoa)
//  - fatigue_score = 0.55*PERCLOS + 0.25*yawn_rate + 0.20*pitch_dev
// Tất cả ngưỡng đều chỉnh được runtime qua console `drowsy set`.
// ============================================================================

#include <cstdint>

// ---- Các trạng thái (đồng bộ với alarm_control) ----
enum class DrowsyState : int {
    DISTRACTED = 4,
    AWAKE = 0,
    PRE_DROWSY = 1, // cảnh báo nhẹ: LED nhấp 1Hz
    DROWSY = 2,     // cảnh báo: LED 2Hz + còi beep định kỳ
    MICROSLEEP = 3, // khẩn cấp: LED sáng + còi liên tục
};

// ---- Tham số có thể chỉnh runtime ----
struct DrowsyParams {
    float distract_yaw_th;
    uint32_t distract_ms;
    uint32_t face_lost_ms;
    float ear_blink_th;      // EAR < ngưỡng này = mắt nhắm (blink)
    float ear_drowsy_th;     // EAR < ngưỡng này = mắt nhắm sâu (drowsy evidence)
    float mar_th;            // MAR > ngưỡng này = miệng mở (ngáp)
    uint32_t microsleep_ms;  // nhắm liên tục >= ngưỡng = MICROSLEEP
    float perclos_th;        // PERCLOS ngưỡng drowsy (0..1)
    uint32_t window_ms;      // cửa sổ trượt PERCLOS
    uint32_t blink_rate_high;// blink/phút cao bất thường
    float pitch_dev_th;      // độ lệch pitch so với baseline
};

class DrowsinessDetector {
public:
    DrowsinessDetector();

    /** @brief Reset toàn bộ trạng thái + baseline. */
    void reset();

    /**
     * @brief Áp dụng tham số mới (từ admin web / console) an toàn.
     */
    void apply_params(const DrowsyParams &p) { m_params = p; }

    /**
     * @brief Cập nhật state machine với 1 frame mới.
     * @param face_detected Có khuôn mặt trong frame không
     * @param ear           Eye Aspect Ratio (0..~0.5)
     * @param mar           Mouth Aspect Ratio (0..~1)
     * @param pitch         Tỷ lệ pitch hình học (frontal ~0.33-0.45)
     * @param now_ms        Thời gian hiện tại (esp_timer_get_time()/1000)
     */
    DrowsyState update(bool face_detected, float ear, float mar, float pitch, float yaw, uint64_t now_ms);

    // ---- Truy vấn số liệu (cho log + console) ----
    DrowsyState state() const { return m_state; }
    float perclos() const { return m_perclos; }           // 0..1
    uint32_t blink_count() const { return m_blink_count; }
    float blink_rate() const;                             // nhịp/phút
    uint32_t yawn_count() const { return m_yawn_count; }
    float yawn_rate() const;                              // lần/phút
    float fatigue_score() const { return m_fatigue_score; } // 0..1
    float pitch_dev() const { return m_pitch_dev; }
    float ear() const { return m_last_ear; }
    float mar() const { return m_last_mar; }
    bool eyes_closed() const { return m_eye_closed; }
    bool yawning() const { return m_yawn_armed; }
    bool attention_off() const { return m_attention_off; }
    uint32_t attention_off_ms() const;
    float yaw_dev() const { return m_yaw_dev; }
    float yaw_baseline() const { return m_yaw_baseline; }

    DrowsyParams &params() { return m_params; }

private:
    DrowsyParams m_params;

    DrowsyState m_state = DrowsyState::AWAKE;

    // ---- theo dõi mắt ----
    bool m_eye_closed = false;      // EAR < ear_blink_th
    bool m_eye_deep_closed = false; // EAR < ear_drowsy_th
    bool m_eye_seen = false;        // đã có mẫu mắt hợp lệ đầu tiên
    uint64_t m_closed_since = 0;    // thời điểm bắt đầu nhắm mắt
    uint64_t m_deep_closed_since = 0;
    uint64_t m_open_since = 0;      // thời điểm mắt mở lại (chống rung state)

    // ---- blink ----
    uint32_t m_blink_count = 0;
    uint64_t m_blink_start = 0;
    bool m_blink_armed = false;
    static const int BLINK_HIST_MAX = 64;      // fix B2: lịch sử thời điểm blink
    uint64_t m_blink_times[BLINK_HIST_MAX] = {0}; // (gần nhất) để tính rate thật
    int m_blink_hist_n = 0;

    // ---- yawn ----
    uint32_t m_yawn_count = 0;
    uint64_t m_yawn_start = 0;
    bool m_yawn_armed = false;
    static const int YAWN_HIST_MAX = 32;       // fix B2
    uint64_t m_yawn_times[YAWN_HIST_MAX] = {0};
    int m_yawn_hist_n = 0;

    // ---- PERCLOS ring buffer (1 mẫu/frame) ----
    bool m_attention_off = false;
    uint64_t m_attention_off_since = 0;
    uint64_t m_face_missing_since = 0;
    float m_yaw_baseline = 0;
    float m_yaw_acc = 0;
    float m_yaw_filtered = 0;
    float m_yaw_dev = 0;
    int m_yaw_n = 0;
    bool m_yaw_ready = false;
    bool m_yaw_filter_valid = false;

    static const int PERCLOS_SAMPLES = 1024;
    bool m_closed_samples[PERCLOS_SAMPLES];
    uint64_t m_closed_times[PERCLOS_SAMPLES]; // ms
    int m_sample_idx = 0;
    int m_sample_count = 0;

    // ---- pitch baseline (học khi AWAKE) ----
    float m_pitch_baseline = 0.35f;
    bool m_pitch_ready = false;
    float m_pitch_acc = 0;
    int m_pitch_n = 0;
    float m_pitch_dev = 0;

    // ---- số liệu hiện tại ----
    float m_perclos = 0;
    float m_fatigue_score = 0;
    float m_last_ear = 0;
    float m_last_mar = 0;
    uint64_t m_last_update_ms = 0;

    // ---- nội bộ ----
    void push_perclos_sample(bool closed, uint64_t now_ms);
    void update_fatigue(uint64_t now_ms);
};
