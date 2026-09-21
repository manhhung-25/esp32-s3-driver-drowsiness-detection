# Benchmark kỹ thuật

## Dữ liệu hiện có

- `2026-09-09_phone_ground_truth.csv`: 9 khoảng ground truth gán thủ công (5 yawn, 2 eyes_closed, 2 distracted).
- `bao_cao_danh_gia_he_thong_buong_ngu.pdf`: báo cáo đã render.
- `bao_cao_danh_gia_he_thong_buong_ngu.docx`: bản nguồn của báo cáo.
- `../tools/evaluate_benchmark.py`: tính confusion matrix, precision, recall, F1, event recall, false-alarm event/hour và latency.

## Kết quả phiên 09/09/2026

Phạm vi chấm điểm 91,7 giây, 335 mẫu AI, face + landmark coverage 99,4%.

| Label | Precision | Recall | F1 | Event recall | Latency p50/p95 |
|---|---:|---:|---:|---:|---:|
| eyes_closed | 68,3% | 63,1% | 65,6% | 2/2 | 627/762 ms |
| yawn | 100,0% | 72,8% | 84,3% | 5/5 | 649/1.012 ms |
| distracted | 97,4% | 90,5% | 93,8% | 2/2 | 642/1.062 ms |

Sai số đồng bộ video/log ước tính khoảng ±0,5 giây. Đây là thử nghiệm nội bộ một người và chưa đủ dài để ngoại suy false-alarm/hour.

## Chạy lại

Trên serial console:

```text
drowsy bench start
drowsy bench mark eyes_start
drowsy bench mark yawn_start
drowsy bench mark distracted_start
drowsy bench stop
```

Lưu toàn bộ serial output thành file log, cập nhật CSV theo uptime ESP32, rồi chạy:

```bash
python tools/evaluate_benchmark.py \
  --log benchmark_results/session.log \
  --ground-truth benchmark_results/2026-09-09_phone_ground_truth.csv
```

## Giao thức benchmark A/B với Raspberry Pi 5

Để kết luận accuracy tương đương, hai hệ thống phải dùng:

1. cùng video đầu vào và cùng khoảng ground truth;
2. cùng định nghĩa event/threshold và cùng cách đồng bộ thời gian;
3. báo cáo cả frame-level F1, event recall, latency và false-alarm/hour;
4. ít nhất nhiều người, nhiều điều kiện sáng, có/không kính và các góc quay đầu;
5. tách kết quả float/INT8 để đo ảnh hưởng quantization, không gộp với khác biệt phần cứng.

Chỉ công bố “tương đương” khi chênh lệch nằm trong biên thống kê đã xác định trước.
