// ============================================================================
// console_cmds.cpp - Lệnh `drowsy` qua esp_console (USB-Serial/JTAG)
// ============================================================================

#include "console_cmds.hpp"

#include <stdio.h>
#include <stdlib.h> // atof/atoi
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "console";

static DrowsinessDetector *s_detector = NULL;
static device_config_t *s_cfg = NULL; // fix C5: để `drowsy set` lưu NVS

static const char *state_str(DrowsyState s)
{
    switch (s) {
    case DrowsyState::AWAKE: return "AWAKE";
    case DrowsyState::PRE_DROWSY: return "PRE_DROWSY";
    case DrowsyState::DROWSY: return "DROWSY";
    case DrowsyState::MICROSLEEP: return "MICROSLEEP";
    }
    return "?";
}

// ---- bảng tên tham số -> con trỏ (để `drowsy set`) ----
// LƯU Ý: mảng này được điền trong register_drowsy_commands() vì cần con trỏ
// detector hợp lệ (không được khởi tạo tĩnh với s_detector==NULL).
struct param_entry_t {
    const char *name;
    void *ptr;
    bool is_float;
};

static param_entry_t s_params[8];
static int s_params_count = 0;

static void fill_param_table(DrowsinessDetector *det)
{
    DrowsyParams &p = det->params();
    s_params[0] = {"ear_blink_th", &p.ear_blink_th, true};
    s_params[1] = {"ear_drowsy_th", &p.ear_drowsy_th, true};
    s_params[2] = {"mar_th", &p.mar_th, true};
    s_params[3] = {"microsleep_ms", &p.microsleep_ms, false};
    s_params[4] = {"perclos_th", &p.perclos_th, true};
    s_params[5] = {"window_ms", &p.window_ms, false};
    s_params[6] = {"blink_rate_high", &p.blink_rate_high, false};
    s_params[7] = {"pitch_dev_th", &p.pitch_dev_th, true};
    s_params_count = 8;
}

static int drowsy_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: drowsy <get|set|stats|reset>\n");
        return 0;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "get") == 0) {
        DrowsyParams &p = s_detector->params();
        printf("--- Drowsiness thresholds ---\n");
        printf("  ear_blink_th    = %.3f\n", p.ear_blink_th);
        printf("  ear_drowsy_th   = %.3f\n", p.ear_drowsy_th);
        printf("  mar_th          = %.3f\n", p.mar_th);
        printf("  microsleep_ms   = %u\n", (unsigned)p.microsleep_ms);
        printf("  perclos_th      = %.2f (%.0f%%)\n", p.perclos_th, p.perclos_th * 100.0f);
        printf("  window_ms       = %u\n", (unsigned)p.window_ms);
        printf("  blink_rate_high = %u\n", (unsigned)p.blink_rate_high);
        printf("  pitch_dev_th    = %.3f\n", p.pitch_dev_th);
        printf("  state           = %s\n", state_str(s_detector->state()));
        return 0;
    }

    if (strcmp(sub, "set") == 0 && argc == 4) {
        const char *name = argv[2];
        for (int i = 0; i < s_params_count; ++i) {
            if (strcmp(name, s_params[i].name) == 0) {
                if (s_params[i].is_float) {
                    *(float *)s_params[i].ptr = (float)atof(argv[3]);
                } else {
                    *(uint32_t *)s_params[i].ptr = (uint32_t)atoi(argv[3]);
                }
                // fix C5: lưu NVS để giữ qua reboot
                if (s_cfg != NULL) {
                    s_cfg->ear_blink_th_x1000 = (int32_t)(s_detector->params().ear_blink_th * 1000.0f);
                    s_cfg->ear_drowsy_th_x1000 = (int32_t)(s_detector->params().ear_drowsy_th * 1000.0f);
                    s_cfg->mar_th_x1000 = (int32_t)(s_detector->params().mar_th * 1000.0f);
                    s_cfg->perclos_th_x1000 = (int32_t)(s_detector->params().perclos_th * 1000.0f);
                    s_cfg->pitch_dev_th_x1000 = (int32_t)(s_detector->params().pitch_dev_th * 1000.0f);
                    s_cfg->microsleep_ms = (int32_t)s_detector->params().microsleep_ms;
                    s_cfg->window_ms = (int32_t)s_detector->params().window_ms;
                    s_cfg->blink_rate_high = (int32_t)s_detector->params().blink_rate_high;
                    config_store_save(s_cfg);
                    printf("  (đã lưu NVS)\n");
                }
                printf("  %s = %s (applied at runtime)\n", name, argv[3]);
                return 0;
            }
        }
        printf("Unknown param '%s'. Xem danh sách: drowsy get\n", name);
        return 0;
    }

    if (strcmp(sub, "stats") == 0) {
        printf("--- Live stats ---\n");
        printf("  state       = %s\n", state_str(s_detector->state()));
        printf("  ear         = %.3f\n", s_detector->ear());
        printf("  mar         = %.3f\n", s_detector->mar());
        printf("  perclos     = %.1f%%\n", s_detector->perclos() * 100.0f);
        printf("  blink/min   = %.1f (total %u)\n", s_detector->blink_rate(), (unsigned)s_detector->blink_count());
        printf("  yawn/min    = %.1f (total %u)\n", s_detector->yawn_rate(), (unsigned)s_detector->yawn_count());
        printf("  pitch_dev   = %.3f\n", s_detector->pitch_dev());
        printf("  fatigue     = %.2f\n", s_detector->fatigue_score());
        return 0;
    }

    if (strcmp(sub, "reset") == 0) {
        s_detector->reset();
        printf("Detector reset.\n");
        return 0;
    }

    printf("Usage: drowsy <get|set|stats|reset>\n");
    return 0;
}

void register_drowsy_commands(DrowsinessDetector *detector, device_config_t *config)
{
    s_detector = detector;
    s_cfg = config;
    fill_param_table(detector);

    const esp_console_cmd_t cmd = {
        .command = "drowsy",
        .help = "Drowsiness detector: get | set <param> <value> | stats | reset",
        .hint = NULL,
        .func = &drowsy_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    ESP_LOGI(TAG, "Registered 'drowsy' command (get/set/stats/reset).");
}
