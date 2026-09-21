#!/usr/bin/env python3
"""Evaluate ESP32 BENCH serial logs against video-derived ground truth.

Ground truth CSV columns: label,start_ms,end_ms
Labels: eyes_closed, yawn, distracted. Times use ESP32 uptime milliseconds.
"""
import argparse
import csv
import math
import statistics
from collections import defaultdict


LABEL_TO_COLUMN = {
    "eyes_closed": 3,
    "yawn": 4,
    "distracted": 5,
}


def read_samples(path):
    samples = []
    with open(path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            fields = line.strip().split(",")
            if len(fields) != 12 or fields[:2] != ["BENCH", "SAMPLE"]:
                continue
            try:
                samples.append([float(value) for value in fields[2:]])
            except ValueError:
                continue
    if not samples:
        raise ValueError("No BENCH,SAMPLE rows found in the log")
    return samples


def read_truth(path):
    truth = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            label = row["label"].strip()
            if label not in LABEL_TO_COLUMN:
                raise ValueError("Unknown label: " + label)
            start, end = float(row["start_ms"]), float(row["end_ms"])
            if end <= start:
                raise ValueError("end_ms must be greater than start_ms")
            truth[label].append((start, end))
    return truth


def active(intervals, timestamp):
    return any(start <= timestamp <= end for start, end in intervals)


def summarize(label, samples, intervals):
    column = LABEL_TO_COLUMN[label]
    tp = fp = fn = tn = 0
    for row in samples:
        actual = active(intervals, row[0])
        predicted = bool(row[column])
        if actual and predicted: tp += 1
        elif actual: fn += 1
        elif predicted: fp += 1
        else: tn += 1
    precision = tp / (tp + fp) if tp + fp else 0.0
    recall = tp / (tp + fn) if tp + fn else 0.0
    f1 = 2 * precision * recall / (precision + recall) if precision + recall else 0.0
    latencies = []
    for start, end in intervals:
        first = next((row[0] for row in samples if start <= row[0] <= end and row[column]), None)
        if first is not None:
            latencies.append(first - start)
    false_alarm_events = 0
    previous_false_alarm = False
    for row in samples:
        false_alarm = bool(row[column]) and not active(intervals, row[0])
        if false_alarm and not previous_false_alarm:
            false_alarm_events += 1
        previous_false_alarm = false_alarm
    return tp, fp, fn, tn, precision, recall, f1, latencies, false_alarm_events


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True, help="serial monitor text containing BENCH rows")
    parser.add_argument("--ground-truth", required=True, help="CSV annotated from the video")
    parser.add_argument("--from-ms", type=float, help="optional first ESP32 timestamp to score")
    parser.add_argument("--to-ms", type=float, help="optional final ESP32 timestamp to score")
    args = parser.parse_args()
    samples, truth = read_samples(args.log), read_truth(args.ground_truth)
    if args.from_ms is not None:
        samples = [row for row in samples if row[0] >= args.from_ms]
    if args.to_ms is not None:
        samples = [row for row in samples if row[0] <= args.to_ms]
    if not samples:
        raise ValueError("No BENCH samples remain after the timestamp filter")
    duration_h = (samples[-1][0] - samples[0][0]) / 3_600_000.0
    coverage = sum(1 for row in samples if row[1] and row[2]) / len(samples)
    print(f"samples={len(samples)} duration_s={duration_h * 3600:.1f} face_landmark_coverage={coverage:.1%}")
    for label in LABEL_TO_COLUMN:
        if not truth[label]:
            print(f"{label}: no ground-truth intervals")
            continue
        tp, fp, fn, tn, precision, recall, f1, latency, false_alarm_events = summarize(label, samples, truth[label])
        false_alarm_h = false_alarm_events / duration_h if duration_h else 0.0
        event_recall = len(latency) / len(truth[label])
        latency_text = "no true positive"
        if latency:
            p95_index = max(0, math.ceil(len(latency) * .95) - 1)
            latency_text = f"p50={statistics.median(latency):.0f}ms p95={sorted(latency)[p95_index]:.0f}ms"
        print(f"{label}: TP={tp} FP={fp} FN={fn} TN={tn} precision={precision:.1%} recall={recall:.1%} "
              f"F1={f1:.1%} event_recall={event_recall:.1%} "
              f"false_alarm_events_per_h={false_alarm_h:.2f} latency {latency_text}")


if __name__ == "__main__":
    main()
