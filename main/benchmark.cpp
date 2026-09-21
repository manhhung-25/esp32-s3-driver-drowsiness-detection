#include "benchmark.hpp"

#include <stdio.h>
#include <string.h>

static volatile bool s_enabled = false;

struct PendingLatency {
    bool pending = false;
    uint64_t start_ms = 0;
};

static PendingLatency s_eyes;
static PendingLatency s_yawn;
static PendingLatency s_distracted;
static PendingLatency s_drowsy;
static PendingLatency s_microsleep;

static PendingLatency *pending_for_label(const char *label)
{
    if (strcmp(label, "eyes_start") == 0) return &s_eyes;
    if (strcmp(label, "yawn_start") == 0) return &s_yawn;
    if (strcmp(label, "distracted_start") == 0) return &s_distracted;
    if (strcmp(label, "drowsy_start") == 0) return &s_drowsy;
    if (strcmp(label, "microsleep_start") == 0) return &s_microsleep;
    return nullptr;
}

static void check_latency(const char *label, PendingLatency *pending, bool detected, uint64_t now_ms)
{
    if (!pending->pending || !detected) return;
    printf("BENCH,LATENCY,%s,start_ms=%llu,detect_ms=%llu,latency_ms=%llu\n",
           label, (unsigned long long)pending->start_ms,
           (unsigned long long)now_ms,
           (unsigned long long)(now_ms - pending->start_ms));
    pending->pending = false;
}

void benchmark_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        s_eyes.pending = false;
        s_yawn.pending = false;
        s_distracted.pending = false;
        s_drowsy.pending = false;
        s_microsleep.pending = false;
    }
    printf("BENCH,SESSION,%s\n", enabled ? "START" : "STOP");
}

bool benchmark_enabled(void)
{
    return s_enabled;
}

void benchmark_mark(const char *label, uint64_t now_ms)
{
    if (s_enabled && label != nullptr) {
        printf("BENCH,MARK,%llu,%s\n", (unsigned long long)now_ms, label);
        PendingLatency *pending = pending_for_label(label);
        if (pending != nullptr) {
            pending->pending = true;
            pending->start_ms = now_ms;
        }
    }
}

void benchmark_log_sample(uint64_t now_ms, bool face, bool landmarks_ok,
                          bool eyes_closed, bool yawn_active, bool attention_off,
                          int state, float ear, float mar, float yaw_dev)
{
    if (!s_enabled) return;
    printf("BENCH,SAMPLE,%llu,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f\n",
           (unsigned long long)now_ms, face ? 1 : 0, landmarks_ok ? 1 : 0,
           eyes_closed ? 1 : 0, yawn_active ? 1 : 0, attention_off ? 1 : 0,
           state, ear, mar, yaw_dev);

    check_latency("eyes", &s_eyes, eyes_closed, now_ms);
    check_latency("yawn", &s_yawn, yawn_active, now_ms);
    check_latency("distracted", &s_distracted, attention_off, now_ms);
    check_latency("drowsy", &s_drowsy, state == 1 || state == 2 || state == 3, now_ms);
    check_latency("microsleep", &s_microsleep, state == 3, now_ms);
}
