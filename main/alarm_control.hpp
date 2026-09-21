#pragma once

// ============================================================================
// alarm_control.hpp - Cảnh báo GPIO: buzzer (PWM LEDC) + LED
// ----------------------------------------------------------------------------
// Task riêng nhận alarm_event_t qua queue, chạy pattern không-blocking:
//   AWAKE      : LED tắt, còi tắt
//   PRE_DROWSY : LED nhấp 1Hz, còi tắt
//   DROWSY     : LED nhấp 2Hz, còi beep 500ms mỗi 2s
//   MICROSLEEP : LED sáng liên tục, còi kêu 200ms/200ms liên tục
// Pin: CONFIG_DROWSY_LED_GPIO / CONFIG_DROWSY_BUZZER_GPIO (menuconfig)
// ============================================================================

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drowsiness_detector.hpp" // DrowsyState

typedef struct {
    DrowsyState state;
} alarm_event_t;

class AlarmControl {
public:
    /**
     * @brief Khởi tạo GPIO + LEDC + task xử lý alarm.
     * @return QueueHandle_t Queue để gửi alarm_event_t (nhận từ AI task).
     */
    QueueHandle_t init();

    void set_test_state(DrowsyState state);
    void clear_test_state();

private:
    static void alarm_task(void *arg);
};
