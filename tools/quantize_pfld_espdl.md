# Quantize PFLD 98 landmark bằng ESP-PPQ → `.espdl`

Hướng dẫn đầy đủ để tạo `model/pfld_landmarks_98.espdl` — file model **duy nhất**
trong dự án cần tự quantize (2 model face detect đã có sẵn pre-quantized của Espressif).

## 1. Vì sao cần model landmark riêng?

Model face detection của ESP-WHO (MNP01) chỉ trả **5 keypoint** (tâm 2 mắt, mũi,
2 khóe miệng) — **không đủ điểm mí mắt nên không tính được EAR (độ mở mắt)**.
PFLD 98 điểm (WFLW) có **6 điểm/mắt** → tính EAR chuẩn công thức Soukupová & Čech,
input chỉ 112×112, backbone MobileNetV2 → chạy ~40-80ms trên ESP32-S3 (đủ ≥8-12 FPS).

## 2. Chuẩn bị môi trường (máy tính, không cần GPU)

> [!WARNING]
> **esp-ppq chỉ hỗ trợ Python 3.8 – 3.12.** Nếu máy bạn đang dùng Python 3.13/3.14
> (pip báo `No matching distribution found for esp-ppq`) thì **PHẢI** dùng venv
> Python 3.10–3.12:

```bash
# Tự động: tìm Python 3.10-3.12, tạo venv ~/.venvs/esp-ppq + cài toàn bộ deps
./tools/setup_quant_env.sh

# Nếu script báo thiếu Python 3.10-3.12, cài trước (Ubuntu/Debian):
# sudo apt install -y python3.12 python3.12-venv
# rồi chạy lại script.
```

Sau đó **luôn dùng venv** để chạy mọi lệnh trong guide này:

```bash
ESP_PY=~/.venvs/esp-ppq/bin/python
$ESP_PY tools/quantize_pfld.py ...   # thay python bằng $ESP_PY
```

> [!NOTE]
> `esp-ppq` là công cụ quantization chính thức của Espressif (fork của PPQ),
> hỗ trợ export thẳng `.espdl` cho esp-dl v3.x. Target ESP32-S3 = `TargetPlatform.ESPDL_S3_INT8`.
> Nếu không muốn cài Python riêng: dùng Docker image của esp-ppq (`docker build` từ
> repo espressif/esp-ppq) — image đã cấu hình Python đúng.

## 3. Lấy model PFLD ONNX (98 điểm, input 112×112)

**Cách A (khuyên dùng) — Tải tự động (đã kiểm chứng 200 OK):**

```bash
./model/download_models.sh
# -> tải model/pfld.onnx (5.1MB, từ HuggingFace mirror của HiSilicon model zoo)
```

Model này (polarisZhao/PFLD-pytorch, 98 điểm WFLW):
- Input `[1,3,112,112]` RGB, output `[1,196]`
- **Preprocessing: chỉ chia 255 (ToTensor) — input [0,1]** → firmware dùng
  `DROWSY_PFLD_MEAN=0, DROWSY_PFLD_STD=255` (đã là mặc định trong Kconfig)

**Cách B — Có sẵn ONNX khác:** đặt vào `model/pfld.onnx` (bất kỳ PFLD 98 điểm nào).

**Cách C — Export từ PyTorch checkpoint** (kiến trúc tái dựng chính xác source gốc):

```bash
~/.venvs/esp-ppq/bin/python tools/export_pfld_onnx.py -c <checkpoint.pth> -o model/pfld.onnx
```

Script tự kiểm chứng output `[1, 196]`. Nếu checkpoint không khớp kiến trúc,
script cảnh báo và bạn nên dùng ONNX khác (xem mục 6).

## 3b. Model PFLD **68 điểm** (py-feat/pfld) — output [1,136]

Nếu bạn dùng checkpoint `pfld_model_best.pth.tar` từ py-feat/pfld (68 điểm):

```bash
~/.venvs/esp-ppq/bin/python tools/export_pyfeat_pfld68.py \
    -c /tmp/pfld_model_best.pth.tar -o model/pfld68_pyfeat.onnx
```

- Kiến trúc: `conv6_1(16) -> concat(GAP(16)+GAP(32)+conv8(128)) = 176 -> fc(176,136)`
- Input `[1,3,112,112]` RGB, **chỉ chia 255** (giống model 98) → Kconfig
  `DROWSY_PFLD_MEAN=0/STD=255` giữ nguyên.
- **`tools/quantize_pfld.py` tự nhận cả 2 model** (98 → `[1,196]`, 68 → `[1,136]`)
  và tự rewrite head `Gemm` → `Conv 1x1`.

> [!WARNING]
> **Model 68 điểm CHƯA chạy được với firmware mặc định.** `main/landmark_pfld.cpp`
> hiện hardcode index 98 điểm (mắt 60-67/68-75, output bắt buộc 196 phần tử).
> Để test model 68 trên thiết bị cần chỉnh thêm (ngoài phạm vi tools/):
> - `LandmarkPFLD::run()`: cho phép `total == 136`, loop `i < 68`
> - `compute_face_metrics()`: mắt phải dùng `bbox_ratio(lm, 60, 67)` cho cả 2 mắt
>   (model 68 chỉ có 8 điểm/mắt, không có vùng 68-75)
> - `web_draw_overlay()`: vẽ `npts = 68` thay vì 98
>
> Còn **EAR/MAR hoàn toàn tính được** với model 68 (8 điểm/mắt đủ công thức
> bbox-ratio như model 98).

## 4. Chuẩn bị dữ liệu calibration (quan trọng nhất cho chất lượng)

> [!IMPORTANT]
> **calib_data là thư mục BẠN TỰ TẠO**, nằm trong thư mục example:
> ```bash
> cd ~/esp/esp-who/examples/driver_drowsiness_detection
> mkdir calib_data    # <-- bỏ ảnh KHUÔN MẶT vào đây (quét đệ quy cả thư mục con)
> ```

**Yêu cầu: ảnh phải là KHUÔN MẶT ĐẦY ĐỦ** (mắt + mũi + miệng) — vì input PFLD là
face crop 112×112. Script tự crop khuôn mặt lớn nhất (Haar cascade) và bỏ qua
đếm ảnh không có mặt.

> [!WARNING]
> **Dataset Kaggle `drowsy-detection-dataset` (yasharjebraeily) KHÔNG dùng được**
> — nó chứa 2000 ảnh **cận cảnh MẮT ĐƠN LẺ** (145×145 grayscale) cho bài toán
> phân loại mắt mở/đóng, không có khuôn mặt. Calibration bằng ảnh mắt sẽ làm
> sai activation range → landmark hỏng hoàn toàn.

**Nguồn ảnh khuôn mặt nên dùng (chọn 1):**

| Nguồn | Ưu điểm | Cách lấy |
|---|---|---|
| **CelebA qua `datasets`** — ĐÃ KIỂM CHỨNG (ảnh thật, HF staff upload) | ảnh mặt thẳng, chuẩn cho PFLD | Xem lệnh bên dưới |
| **Tự quay bằng camera** (điện thoại/webcam) | Khớp 100% điều kiện lái xe thật | Quay 3-5 phút ở nhiều góc/ánh sáng, tách khung hình (ffmpeg) lấy ~200 ảnh |
| **WFLW gốc (Google Drive)** | Tập train gốc PFLD | `pip install gdown && gdown 1hzBd48JIdWTJSsATBEB_eFVvPL1bx6UC` (~1GB) — Google Drive có thể bị chặn mạng |

> [!CAUTION]
> ⚠️ **`duongnguy/WFLW_augmented` trên HF KHÔNG dùng được** — đã tải thử 270MB, giải nén
> ra **74.950 ảnh ĐEN XÌ** (mean pixel 0.7/255 — đo bằng OpenCV + PIL). Đừng dùng nguồn này.
> Luôn kiểm tra bằng `tools/check_calib.py` (có kiểm tra độ sáng) trước khi quantize.

**Tải 300 ảnh CelebA** (đã kiểm chứng — 286/300 detect được mặt, sáng trung bình 60/255):

```bash
~/.venvs/esp-ppq/bin/pip install datasets
~/.venvs/esp-ppq/bin/python - <<'EOF'
from datasets import load_dataset
import numpy as np, os
os.makedirs('calib_data', exist_ok=True)
ds = load_dataset('nielsr/CelebA-faces', split='train', streaming=True)
saved = 0
for ex in ds:
    img = ex['image'].convert('RGB')
    if np.array(img).mean() < 20:   # bỏ ảnh tối
        continue
    img.save(f'calib_data/celeba_{saved:03d}.png')
    saved += 1
    if saved >= 300: break
print('Đã lưu', saved, 'ảnh')
EOF
```

Số lượng: **100-500 ảnh** là đủ. Ít hơn 50 → chất lượng quantization kém.
Script tự resize 112×112 + chia 255 (khớp firmware `DROWSY_PFLD_MEAN=0/STD=255`).

> [!TIP]
> **Ảnh mặt đã crop sẵn (WFLW 112×112) → thêm `--no-face-crop`** để dùng nguyên ảnh
> (không cần Haar detect — ảnh vốn đã là face crop đúng chuẩn input PFLD).
> `tools/check_calib.py` tự nhận diện trường hợp này.

## 5. Chạy quantization

**Model 98 điểm:**

```bash
~/.venvs/esp-ppq/bin/python tools/quantize_pfld.py \
    -i model/pfld.onnx \
    -o model/pfld_landmarks_98.espdl \
    --calib-dir calib_data \
    --calib-steps 128
```

**Model 68 điểm (py-feat/pfld):**

```bash
~/.venvs/esp-ppq/bin/python tools/quantize_pfld.py \
    -i model/pfld68_pyfeat.onnx \
    -o model/pfld68_pyfeat.espdl \
    --calib-dir calib_data \
    --calib-steps 128
```

- Mất vài phút trên CPU (không cần GPU).
- Script **tự động nhận cả 98 ([1,196]) lẫn 68 ([1,136]) điểm** và:
  - Cắt output phụ (model có thêm feature map) — chỉ giữ output landmark
  - Thay head `GAP+Reshape+Concat+Gemm` bằng `GlobalAveragePool+Concat+Conv1x1`
    (workaround lỗi esp-ppq 1.3.x với GAP+Concat 1D+Gemm; GAP **giữ nguyên kênh C**
    — fix lỗi "Conv fail: C:130 kernel channels:176")
- `export_test_values=True` được bật sẵn → model nhúng giá trị test, có thể gọi
  `model.test()` trên thiết bị để xác minh inference đúng.
- Kết quả: `model/pfld_landmarks_98.espdl` (~1.3MB) hoặc `model/pfld68_pyfeat.espdl` (int8).

> [!IMPORTANT]
> **Chất lượng phụ thuộc hoàn toàn vào dữ liệu calibration.** Không có `--calib-dir`
> (hoặc thư mục trống) script vẫn chạy được với dữ liệu ngẫu nhiên nhưng model
> sẽ KHÔNG chính xác — chỉ dùng để test pipeline. Phải có 100-500 ảnh khuôn mặt
> thật (điều kiện lái xe: sáng/tối/đeo kính...) để có model dùng được.

## 6. Kiểm chứng mapping landmark TRƯỚC khi chạy thiết bị

```bash
python tools/verify_pfld_mapping.py -m model/pfld.onnx -i ảnh_có_mặt.jpg
# model 68 điểm:
python tools/verify_pfld_mapping.py -m model/pfld68_pyfeat.onnx -i ảnh_có_mặt.jpg
```

Script **tự nhận số điểm từ output size** (196 → 98 điểm, 136 → 68 điểm).
Ảnh `result_landmarks.jpg` vẽ N điểm kèm index.

**Model 98 (mặc định, pfld-sim.onnx / polarisZhao):**
- Mắt (đỏ): index **60-75** (60-67 mắt trái, 68-75 mắt phải, 8 điểm/mắt)
- Mũi (xanh): **51-59**
- Miệng (vàng): **76-97**

**Model 68 (py-feat/pfld, iBUG/300-W):**
- Mắt (đỏ): **36-41** (trái) / **42-47** (phải)
- Mũi (xanh): **27-35**
- Miệng (vàng): **48-67** (48-59 viền ngoài)

> [!NOTE]
> Đã xác minh bằng output thật (toạ độ landmark 27-35 nằm dọc mũi, 36-41/42-47
> nằm 2 bên mắt, 48-67 quanh miệng). Firmware mặc định dùng model 68
> (Kconfig `DROWSY_PFLD_68PT`) — EAR = bbox_ratio(36-41) + bbox_ratio(42-47).

> [!WARNING]
> Nếu bạn đổi model khác và mapping lệch, **bắt buộc** chỉnh các hằng số index
> trong hàm `compute_face_metrics()` (`landmark_pfld.cpp`) trước khi dùng thật
> (chỉ là số nguyên - KHÔNG cần train lại). EAR/MAR dùng tỷ lệ bounding box
> h/w nên không phụ thuộc thứ tự điểm bên trong từng mắt.

## 7. Build & chạy

```bash
cd examples/driver_drowsiness_detection
idf.py build flash monitor
```

Firmware nhúng sẵn 3 file .espdl (CMake báo lỗi rõ nếu thiếu) — **flash 1 lệnh là chạy**.

## 8. Nâng cao: tinh chỉnh quantization

- `--calib-steps` tăng lên 256-512 nếu độ chính xác chưa đạt.
- Thử `num_of_bits=16` (chậm hơn ~30% nhưng chính xác hơn) nếu mắt đeo kính
  làm EAR nhiễu.
- ESP-PPQ hỗ trợ các pass phục hồi độ chính xác (LSQ, bias correction) — tham khảo
  docs esp-ppq nếu cần.
- Kiểm tra op hỗ trợ: PFLD chỉ dùng Conv/DWConv/BN/ReLU6/GAP/Gemm — tất cả đều
  được esp-dl v3 hỗ trợ (xem `operator_support_state.md` trong repo esp-dl).
