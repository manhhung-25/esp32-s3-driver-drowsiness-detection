// ============================================================================
// alarm_control.cpp - Điều khiển buzzer + LED theo pattern
// ============================================================================

#include "alarm_control.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "alarm_control";

#define BUZZER_FREQ_HZ   2000 // tần số PWM còi (2kHz nghe rõ nhất với buzzer thường)
#define BUZZER_DUTY_MAX  4095 // 12-bit
#define ALARM_QUEUE_SIZE 5

// LƯU Ý: camera XCLK đã dùng LEDC_CHANNEL_0 + LEDC_TIMER_0 (esp32-camera)
// -> buzzer PHẢI dùng channel/timer khác để tránh xung đột!
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1
#define BUZZER_LEDC_TIMER   LEDC_TIMER_1

static QueueHandle_t s_alarm_queue = NULL;

// ---- LED: đơn giản on/off ----
static void led_set(bool on)
{
    gpio_set_level((gpio_num_t)CONFIG_DROWSY_LED_GPIO, on ? 1 : 0);
}

// ---- Buzzer: PWM qua LEDC ----
static void buzzer_set(bool on)
{
    if (on) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, BUZZER_DUTY_MAX / 2); // 50%
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    }
}

// ---- Pattern state (không-blocking, tick 10ms) ----
static void apply_pattern(DrowsyState state, uint32_t tick_ms)
{
    switch (state) {
    case DrowsyState::AWAKE:
        led_set(false);
        buzzer_set(false);
        break;

    case DrowsyState::PRE_DROWSY:
        // LED 1Hz (500ms on / 500ms off)
        led_set((tick_ms % 1000) < 500);
        buzzer_set(false);
        break;

    case DrowsyState::DROWSY:
        // LED 2Hz (250ms on / 250ms off)
        led_set((tick_ms % 500) < 250);
        // Còi beep 500ms mỗi 2 giây
        buzzer_set((tick_ms % 2000) < 500);
        break;

    case DrowsyState::MICROSLEEP:
        // LED sáng liên tục + còi 200ms/200ms liên tục (rất to, đánh thức)
        led_set(true);
        buzzer_set((tick_ms % 400) < 200);
        break;
    }
}

void AlarmControl::alarm_task(void *arg)
{
    DrowsyState current = DrowsyState::AWAKE;
    uint32_t tick_ms = 0;
    const TickType_t tick_period = pdMS_TO_TICKS(10); // tick 10ms

    while (true) {
        alarm_event_t ev;
        // Nhận sự kiện mới (nếu có), không chặn để pattern chạy liên tục
        if (xQueueReceive(s_alarm_queue, &ev, 0) == pdTRUE) {
            current = ev.state;
        }
        apply_pattern(current, tick_ms);
        tick_ms += 10;
        vTaskDelay(tick_period);
    }
}

QueueHandle_t AlarmControl::init()
{
    // ---- LED GPIO ----
    gpio_config_t led_conf = {};
    led_conf.mode = GPIO_MODE_OUTPUT;
    led_conf.pin_bit_mask = 1ULL << CONFIG_DROWSY_LED_GPIO;
    led_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    led_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    led_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&led_conf);
    led_set(false);

    // ---- Buzzer LEDC PWM ----
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.timer_num = BUZZER_LEDC_TIMER;
    timer_conf.duty_resolution = LEDC_TIMER_12_BIT;
    timer_conf.freq_hz = BUZZER_FREQ_HZ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {};
    ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_conf.channel = BUZZER_LEDC_CHANNEL;
    ch_conf.timer_sel = BUZZER_LEDC_TIMER;
    ch_conf.intr_type = LEDC_INTR_DISABLE;
    ch_conf.gpio_num = CONFIG_DROWSY_BUZZER_GPIO;
    ch_conf.duty = 0;
    ch_conf.hpoint = 0;
    ledc_channel_config(&ch_conf);
    buzzer_set(false);

    // ---- Queue + task alarm (Core 1, cùng core AI để tránh đua queue) ----
    s_alarm_queue = xQueueCreate(ALARM_QUEUE_SIZE, sizeof(alarm_event_t));
    xTaskCreatePinnedToCore(alarm_task, "alarm_task", 3 * 1024, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Alarm ready: LED=GPIO%d, Buzzer=GPIO%d", CONFIG_DROWSY_LED_GPIO, CONFIG_DROWSY_BUZZER_GPIO);
    return s_alarm_queue;
}
