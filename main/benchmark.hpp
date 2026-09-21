#pragma once

#include <stdbool.h>
#include <stdint.h>

// Compact CSV records for offline comparison with video ground truth.
void benchmark_set_enabled(bool enabled);
bool benchmark_enabled(void);
void benchmark_mark(const char *label, uint64_t now_ms);
// Mark labels: eyes_start, yawn_start, distracted_start, drowsy_start, microsleep_start.
// The next matching AI detection prints BENCH,LATENCY with the measured delay.
void benchmark_log_sample(uint64_t now_ms, bool face, bool landmarks_ok,
                          bool eyes_closed, bool yawn_active, bool attention_off,
                          int state, float ear, float mar, float yaw_dev);
