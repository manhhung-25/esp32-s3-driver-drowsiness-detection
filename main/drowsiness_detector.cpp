// ============================================================================
// drowsiness_detector.cpp - State machine phát hiện buồn ngủ
// ============================================================================

#include "drowsiness_detector.hpp"

#include <algorithm>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "eye_timing";

// Hằng số nội bộ (không cần chỉnh ngoài Kconfig)
#define BLINK_MAX_MS      800u   // nhắm mắt > 800ms không tính là blink (ngủ gật)
#define BLINK_MIN_MS      50u    // nhắm quá ngắn = nhiễu
#define YAWN_MIN_MS       1000u  // miệng mở >= 1s mới tính là ngáp
#define MICROSLEEP_RECOVER_MS 3000u // mắt mở liên tục >= 3s mới thoát microsleep
#define DROWSY_RECOVER_MS     10000u // mắt mở >= 10s mới về AWAKE
#define AWAKE_RECOVER_PERCLOS 0.2f // PERCLOS < 20% mới về AWAKE

DrowsinessDetector::DrowsinessDetector()
{
    m_params.distract_yaw_th = CONFIG_DROWSY_DISTRACT_YAW_TH / 1000.0f;
    m_params.distract_ms = CONFIG_DROWSY_DISTRACT_MS;
    m_params.face_lost_ms = CONFIG_DROWSY_FACE_LOST_MS;
    // Nạp giá trị mặc định từ Kconfig (menuconfig) - vẫn chỉnh runtime được.
    // LƯU Ý: Kconfig KHÔNG có kiểu float -> ngưỡng dạng tỷ lệ lưu dạng int
    // ĐƠN VỊ MILLI (x1000) -> chia /1000 khi nạp vào tham số float.
    m_params.ear_blink_th = CONFIG_DROWSY_EAR_BLINK_TH / 1000.0f;
    m_params.ear_drowsy_th = CONFIG_DROWSY_EAR_DROWSY_TH / 1000.0f;
    m_params.mar_th = CONFIG_DROWSY_MAR_TH / 1000.0f;
    m_params.microsleep_ms = CONFIG_DROWSY_MICROSLEEP_MS;
    m_params.perclos_th = CONFIG_DROWSY_PERCLOS_TH / 1000.0f; // 400 = 40%
    m_params.window_ms = CONFIG_DROWSY_WINDOW_MS;
    m_params.blink_rate_high = CONFIG_DROWSY_BLINK_RATE_HIGH;
    m_params.pitch_dev_th = CONFIG_DROWSY_PITCH_DEV_TH / 1000.0f;
    reset();
}

void DrowsinessDetector::reset()
{
    m_state = DrowsyState::AWAKE;
    m_eye_closed = false;
    m_eye_deep_closed = false;
    m_eye_seen = false;
    m_closed_since = 0;
    m_deep_closed_since = 0;
    m_open_since = 0;
    m_blink_count = 0;
    m_blink_start = 0;
    m_blink_armed = false;
    m_blink_hist_n = 0;
    m_yawn_count = 0;
    m_yawn_start = 0;
    m_yawn_armed = false;
    m_yawn_hist_n = 0;
    m_attention_off = false;
    m_attention_off_since = 0;
    m_face_missing_since = 0;
    m_yaw_baseline = 0;
    m_yaw_acc = 0;
    m_yaw_filtered = 0;
    m_yaw_dev = 0;
    m_yaw_n = 0;
    m_yaw_ready = false;
    m_yaw_filter_valid = false;
    m_sample_idx = 0;
    m_sample_count = 0;
    m_pitch_baseline = 0.35f;
    m_pitch_ready = false;
    m_pitch_acc = 0;
    m_pitch_n = 0;
    m_pitch_dev = 0;
    m_perclos = 0;
    m_fatigue_score = 0;
    m_last_update_ms = 0;
    for (int i = 0; i < PERCLOS_SAMPLES; ++i) {
        m_closed_samples[i] = false;
        m_closed_times[i] = 0;
    }
}

float DrowsinessDetector::blink_rate() const
{
    // fix B2: rate THẬT trong cửa sổ trượt (không cộng dồn từ reset).
    // Đếm blink có timestamp trong [now - window, now] rồi quy ra nhịp/phút.
    uint32_t window = std::max<uint32_t>(m_params.window_ms, 1000u);
    uint64_t now = m_last_update_ms;
    uint64_t start = (now > window) ? now - window : 0;
    int cnt = 0;
    for (int i = 0; i < m_blink_hist_n; ++i) {
        if (m_blink_times[i] >= start) cnt++;
    }
    return cnt * 60000.0f / (float)window;
}

float DrowsinessDetector::yawn_rate() const
{
    uint32_t window = std::max<uint32_t>(m_params.window_ms, 1000u);
    uint64_t now = m_last_update_ms;
    uint64_t start = (now > window) ? now - window : 0;
    int cnt = 0;
    for (int i = 0; i < m_yawn_hist_n; ++i) {
        if (m_yawn_times[i] >= start) cnt++;
    }
    return cnt * 60000.0f / (float)window;
}

uint32_t DrowsinessDetector::attention_off_ms() const
{
    if (!m_attention_off || m_last_update_ms < m_attention_off_since) return 0;
    const uint64_t elapsed = m_last_update_ms - m_attention_off_since;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

// ---- Ghi mẫu "mắt nhắm?" vào ring buffer cho PERCLOS ----
void DrowsinessDetector::push_perclos_sample(bool closed, uint64_t now_ms)
{
    m_closed_samples[m_sample_idx] = closed;
    m_closed_times[m_sample_idx] = now_ms;
    m_sample_idx = (m_sample_idx + 1) % PERCLOS_SAMPLES;
    if (m_sample_count < PERCLOS_SAMPLES) {
        m_sample_count++;
    }

    // Tính PERCLOS trong cửa sổ trượt [now - window, now]
    uint32_t window = m_params.window_ms;
    uint64_t start_time = (now_ms > window) ? now_ms - window : 0;
    int closed_count = 0;
    int total = 0;
    for (int i = 0; i < m_sample_count; ++i) {
        if (m_closed_times[i] >= start_time) {
            total++;
            if (m_closed_samples[i]) {
                closed_count++;
            }
        }
    }
    m_perclos = (total > 0) ? (float)closed_count / (float)total : 0.0f;
}

// ---- Điểm mệt mỏi tổng hợp ----
void DrowsinessDetector::update_fatigue(uint64_t now_ms)
{
    (void)now_ms; // fix M5: giữ chữ ký đồng bộ (now_ms không dùng trực tiếp ở đây)
    float perclos_norm = std::min(1.0f, m_perclos / std::max(m_params.perclos_th, 0.01f));
    float yawn_norm = std::min(1.0f, yawn_rate() / 3.0f);          // 3 ngáp/phút = max
    float pitch_norm = std::min(1.0f, m_pitch_dev / std::max(m_params.pitch_dev_th, 0.01f));
    // fix D1: dùng blink_rate_high - blink nhanh bất thường (>= ngưỡng) góp điểm mệt
    float blink_norm = std::min(1.0f, blink_rate() / std::max((float)m_params.blink_rate_high, 1.0f));

    m_fatigue_score = 0.40f * perclos_norm + 0.20f * yawn_norm +
                      0.20f * pitch_norm + 0.20f * blink_norm;
}

DrowsyState DrowsinessDetector::update(bool face_detected, float ear, float mar, float pitch, float yaw, uint64_t now_ms)
{
    m_last_ear = ear;
    m_last_mar = mar;
    m_last_update_ms = now_ms;

    // ---------- 1. Attention: yaw relative to the driver's centered baseline ----------
    // PFLD's raw nose offset varies with camera placement and facial geometry. Learn the
    // centered pose first, then evaluate a filtered deviation instead of absolute yaw.
    if (face_detected && isfinite(yaw)) {
        if (!m_yaw_filter_valid) {
            m_yaw_filtered = yaw;
            m_yaw_filter_valid = true;
        } else {
            m_yaw_filtered += 0.35f * (yaw - m_yaw_filtered);
        }
        m_face_missing_since = 0;

        const bool centered_sample = !m_eye_closed && ear >= m_params.ear_blink_th &&
                                    mar <= m_params.mar_th;
        if (!m_yaw_ready && centered_sample) {
            m_yaw_acc += m_yaw_filtered;
            if (++m_yaw_n >= 20) {
                m_yaw_baseline = m_yaw_acc / (float)m_yaw_n;
                m_yaw_ready = true;
            }
        } else if (m_yaw_ready && centered_sample &&
                   fabsf(m_yaw_filtered - m_yaw_baseline) < m_params.distract_yaw_th * 0.5f) {
            m_yaw_baseline += 0.01f * (m_yaw_filtered - m_yaw_baseline);
        }
        m_yaw_dev = m_yaw_ready ? fabsf(m_yaw_filtered - m_yaw_baseline) : 0.0f;
    } else {
        if (m_face_missing_since == 0) m_face_missing_since = now_ms;
        m_yaw_dev = 0.0f;
    }

    const bool face_missing = !face_detected &&
                              (now_ms - m_face_missing_since) >= m_params.face_lost_ms;
    const bool yaw_away = face_detected && m_yaw_ready &&
                          m_yaw_dev >= m_params.distract_yaw_th;
    const bool attention_now_off = face_missing || yaw_away;
    if (attention_now_off) {
        if (!m_attention_off) {
            m_attention_off_since = face_missing ? m_face_missing_since : now_ms;
        }
        m_attention_off = true;
    } else {
        m_attention_off = false;
        m_attention_off_since = 0;
    }

    if (!face_detected) {
        // Mất mặt: không tính là nhắm mắt (tránh false positive khi quay đầu).
        // KHÔNG reset m_open_since ở đây — chỉ reset khi thực sự quan sát mắt MỞ
        // (có mặt). Nếu reset mỗi frame mất mặt sẽ khiến eyes_open_since luôn ~0
        // -> MICROSLEEP không bao giờ thoát (bug "treo trạng thái").
        m_eye_closed = false;
        m_eye_deep_closed = false;
        push_perclos_sample(false, now_ms);
        update_fatigue(now_ms);
        if (m_state != DrowsyState::MICROSLEEP && m_state != DrowsyState::DROWSY &&
            attention_off_ms() >= m_params.distract_ms) {
            m_state = DrowsyState::DISTRACTED;
        }
        return m_state;
    }

    bool closed = (ear < m_params.ear_blink_th);
    bool deep_closed = (ear < m_params.ear_drowsy_th);
    const bool was_closed = m_eye_closed;

    // ---- blink detection (đóng mở nhanh) ----
    if (closed && !m_eye_closed) {
        m_blink_armed = true;
        m_blink_start = now_ms;
        m_closed_since = now_ms;
        ESP_LOGI(TAG, "EYE_DETECT,CLOSED,t_ms=%llu,ear=%.3f",
                 (unsigned long long)now_ms, ear);
    } else if (!closed && m_eye_closed && m_blink_armed) {
        uint64_t dur = now_ms - m_blink_start;
        ESP_LOGI(TAG, "EYE_DETECT,OPEN,t_ms=%llu,ear=%.3f,closed_ms=%llu,blink=%d",
                 (unsigned long long)now_ms, ear,
                 (unsigned long long)(m_closed_since ? now_ms - m_closed_since : dur),
                 (dur >= BLINK_MIN_MS && dur <= BLINK_MAX_MS) ? 1 : 0);
        if (dur >= BLINK_MIN_MS && dur <= BLINK_MAX_MS) {
            m_blink_count++;
            // fix B2: lưu timestamp để tính rate trong cửa sổ (ring nhỏ)
            if (m_blink_hist_n < BLINK_HIST_MAX) {
                m_blink_times[m_blink_hist_n++] = now_ms;
            } else {
                // dịch trái 1 - giữ 64 gần nhất
                for (int i = 1; i < BLINK_HIST_MAX; ++i) m_blink_times[i - 1] = m_blink_times[i];
                m_blink_times[BLINK_HIST_MAX - 1] = now_ms;
            }
        }
        m_blink_armed = false;
    } else if (!closed && !m_eye_seen) {
        // Ghi nhận trạng thái ban đầu để terminal cho biết hệ thống đã thấy mắt mở.
        ESP_LOGI(TAG, "EYE_DETECT,OPEN,t_ms=%llu,ear=%.3f,closed_ms=0,blink=0",
                 (unsigned long long)now_ms, ear);
    }

    // ---- theo dõi nhắm sâu (microsleep) ----
    if (deep_closed) {
        if (!m_eye_deep_closed) {
            m_deep_closed_since = now_ms;
        }
    }
    if (closed) {
        // m_closed_since được gán tại nhánh chuyển sang CLOSED ở trên.
    } else if (was_closed || m_open_since == 0) {
        m_open_since = now_ms;
    }

    m_eye_closed = closed;
    m_eye_deep_closed = deep_closed;
    m_eye_seen = true;

    // ---- yawn detection (miệng mở lâu) ----
    bool mouth_open = (mar > m_params.mar_th);
    if (mouth_open && !m_yawn_armed) {
        m_yawn_armed = true;
        m_yawn_start = now_ms;
    } else if (!mouth_open && m_yawn_armed) {
        if (now_ms - m_yawn_start >= YAWN_MIN_MS) {
            m_yawn_count++;
            // fix B2
            if (m_yawn_hist_n < YAWN_HIST_MAX) {
                m_yawn_times[m_yawn_hist_n++] = now_ms;
            } else {
                for (int i = 1; i < YAWN_HIST_MAX; ++i) m_yawn_times[i - 1] = m_yawn_times[i];
                m_yawn_times[YAWN_HIST_MAX - 1] = now_ms;
            }
        }
        m_yawn_armed = false;
    }

    // ---------- 2. Pitch baseline (chỉ học khi tỉnh táo) ----------
    if (!m_pitch_ready && !closed && !mouth_open && isfinite(pitch)) {
        m_pitch_baseline = pitch;
        m_pitch_ready = true;
    } else if (m_pitch_ready && m_state == DrowsyState::AWAKE &&
               !closed && !mouth_open && !m_attention_off && isfinite(pitch)) {
        // Adapt slowly to normal seating/camera shifts without learning a nod.
        if (fabsf(pitch - m_pitch_baseline) < m_params.pitch_dev_th) {
            m_pitch_baseline += 0.01f * (pitch - m_pitch_baseline);
        }
    }
    m_pitch_dev = m_pitch_ready ? fabsf(pitch - m_pitch_baseline) : 0.0f;

    // ---------- 3. PERCLOS + fatigue score ----------
    push_perclos_sample(closed, now_ms);
    update_fatigue(now_ms);

    // ---------- 4. State machine (hysteresis chống rung) ----------
    bool microsleep = m_eye_deep_closed && (now_ms - m_deep_closed_since) >= m_params.microsleep_ms;
    // LƯU Ý: phải là uint64_t (thời gian mắt mở tính bằng ms) - không phải bool!
    uint64_t eyes_open_since = now_ms - m_open_since;

    switch (m_state) {
    case DrowsyState::AWAKE:
        if (microsleep) {
            m_state = DrowsyState::MICROSLEEP;
        } else if (m_pitch_ready && m_fatigue_score >= 0.6f) {
            m_state = DrowsyState::PRE_DROWSY;
        }
        break;

    case DrowsyState::PRE_DROWSY:
        if (microsleep) {
            m_state = DrowsyState::MICROSLEEP;
        } else if (m_perclos >= m_params.perclos_th || yawn_rate() >= 3.0f) {
            m_state = DrowsyState::DROWSY;
        } else if (!closed && !mouth_open && !m_attention_off &&
                   m_fatigue_score < 0.45f && eyes_open_since >= 1000u) {
            m_state = DrowsyState::AWAKE;
        }
        break;

    case DrowsyState::DROWSY:
        if (microsleep) {
            m_state = DrowsyState::MICROSLEEP;
        } else if (ear >= m_params.ear_blink_th) {
            // fix treo: thoát DROWSY chỉ cần mắt MỞ lại (EAR >= ngưỡng), không đợi
            // timer open_since / perclos — tránh kẹt state khi tài xế tỉnh lại.
            m_state = DrowsyState::AWAKE;
        }
        break;

    case DrowsyState::DISTRACTED:
        if (microsleep) {
            m_state = DrowsyState::MICROSLEEP;
        } else if (!m_attention_off) {
            m_state = m_fatigue_score >= 0.6f ? DrowsyState::PRE_DROWSY : DrowsyState::AWAKE;
        }
        break;

    case DrowsyState::MICROSLEEP:
        // fix treo: thoát MICROSLEEP dựa trực tiếp EAR + MAR (không dùng timer).
        // - Mắt MỞ lại: EAR >= ear_blink_th
        // - Miệng KHÔNG mở/ngáp: MAR <= mar_th
        // Khi mất mặt: ear=0, mar=0 -> không thoát (an toàn, tránh release còi sai).
        if (ear >= m_params.ear_blink_th && mar <= m_params.mar_th) {
            m_state = DrowsyState::AWAKE;
        }
        break;
    }

    if (m_state != DrowsyState::MICROSLEEP && m_state != DrowsyState::DROWSY &&
        m_attention_off && attention_off_ms() >= m_params.distract_ms) {
        m_state = DrowsyState::DISTRACTED;
    }

    return m_state;
}
