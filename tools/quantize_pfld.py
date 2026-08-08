#!/usr/bin/env python3
# ============================================================================
# quantize_pfld.py - Quantize PFLD ONNX -> .espdl bằng ESP-PPQ (ESP32-S3)
# ----------------------------------------------------------------------------
# Pipeline:
#   ONNX (fp32) --strip output phụ--> --calibration--> PPQ int8 --> .espdl
#
# Tự động xử lý:
#   * CẮT các output phụ (vd model PFLD có thêm feature map [1,64,28,28]) -
#     chỉ giữ output landmark [1,196] (bắt buộc cho dl::Model::get_output())
#   * Calibration theo đúng preprocessing model: mặc định ToTensor-style
#     (chỉ chia 255 -> input [0,1]) - khớp Kconfig DROWSY_PFLD_MEAN=0/STD=255.
#
# Cách dùng:
#   ~/.venvs/esp-ppq/bin/python tools/quantize_pfld.py \
#       -i model/pfld.onnx -o model/pfld_landmarks_98.espdl \
#       --calib-dir calib_data --calib-steps 128
#
# Calibration data: thư mục ảnh khuôn mặt (crop sẵn, ~100-500 ảnh) đa dạng
# ánh sáng/góc nhìn. Không có ảnh -> dùng random (chỉ test pipeline).
# ============================================================================

import argparse
import os
import sys
import tempfile

# ----------------------------------------------------------------------------
# KIỂM TRA PYTHON VERSION (esp-ppq chỉ hỗ trợ 3.8 - 3.12)
# ----------------------------------------------------------------------------
if sys.version_info >= (3, 13):
    sys.exit(
        "\nLỖI: esp-ppq chỉ hỗ trợ Python 3.8-3.12, máy bạn đang dùng "
        f"Python {sys.version_info.major}.{sys.version_info.minor}.\n"
        "Giải pháp: chạy ./tools/setup_quant_env.sh để tạo venv Python đúng,\n"
        "rồi dùng: ~/.venvs/esp-ppq/bin/python tools/quantize_pfld.py ..."
    )

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset

# ---- Patch onnxsim.simplify TRƯỚC KHI import esp_ppq ----
# esp_ppq (espdl_interface.py) làm `from onnxsim import simplify` tại import-time
# nên phải gán sys.modules['onnxsim'].simplify trước. Model 68 torchlm/wgs sau
# graph-surgery hợp lệ (checker + onnxruntime OK) nhưng onnxsim C++ lỗi
# 'Inferred shape differ in rank: (1) vs (4)' (bug shape-inference của onnxsim).
# Bỏ simplify: esp-ppq quantize thẳng model đã sạch dead-code.
try:
    import onnxsim
    onnxsim.simplify = lambda model, *a, **k: (model, True)
    import sys as _sys
    _sys.modules['onnxsim'].simplify = onnxsim.simplify
except ImportError:
    pass

from esp_ppq import QuantizationSettingFactory
from esp_ppq.api import espdl_quantize_onnx


# ----------------------------------------------------------------------------
# 1. Tiền xử lý ONNX: cắt output phụ + thay head Gemm bằng Conv 1x1
# ----------------------------------------------------------------------------
def preprocess_onnx(onnx_path: str, landmark_output_name: str = None) -> str:
    """
    a) Giữ lại duy nhất output landmark [1,196]/[1,136] (dl::Model::get_output() cần 1 output)
    b) Thay head GAP+Reshape+Concat+Gemm bằng GlobalAveragePool+Concat+Conv1x1
       - LÝ DO: esp-ppq 1.3.x crash khi Gemm nhận input 1D từ Concat sau GAP
         (lỗi matmul shape). Conv 1x1 là cách diễn đạt FC an toàn nhất.
    """
    import onnx
    from onnx import helper, numpy_helper
    from onnx.utils import extract_model
    import numpy as np

    model = onnx.load(onnx_path)
    graph = model.graph

    # --- a) Tìm output landmark: chấp nhận [1,196] (98 điểm) HOẶC [1,136] (68 điểm) ---
    landmark_size = None
    if landmark_output_name is None:
        for o in graph.output:
            shape = [d.dim_value for d in o.type.tensor_type.shape.dim]
            # Batch có thể là 0 (dynamic) hoặc 1 (static)
            if len(shape) == 2 and shape[0] in (0, 1) and shape[1] in (196, 136):
                landmark_output_name = o.name
                landmark_size = shape[1]
                break
    if landmark_output_name is None:
        sys.exit(
            "LỖI: không tìm thấy output [1,196] (98 điểm) hoặc [1,136] (68 điểm) "
            "trong ONNX. Model của bạn có phải PFLD 68/98?"
        )

    n_outputs = len(graph.output)
    input_names = [i.name for i in graph.input]
    tmp_path = onnx_path + ".preprocessed.onnx"
    extract_model(onnx_path, tmp_path, input_names=input_names, output_names=[landmark_output_name])
    if n_outputs > 1:
        print(f"==> Cắt {n_outputs - 1} output phụ, giữ '{landmark_output_name}' [{landmark_size}]")
    else:
        print(f"==> Output landmark: '{landmark_output_name}' [{landmark_size}]")

    # --- b) Rewrite head Gemm -> Conv 1x1 ---
    model = onnx.load(tmp_path)
    g = model.graph
    gemm = None
    for n in g.node:
        if n.op_type == 'Gemm' and landmark_output_name in n.output:
            gemm = n
            break
    if gemm is None:
        return tmp_path

    concat = None
    for n in g.node:
        if n.op_type == 'Concat' and gemm.input[0] in n.output:
            concat = n
            break
    if concat is None:
        return tmp_path

    producer = {}
    for n in g.node:
        for o in n.output:
            producer[o] = n

    def trace_feature(tname):
        """Truy ngược qua Reshape/Flatten/Squeeze/GAP/ReduceMean/Pad tới feature 4D.
        Model 68 (Gitee) dùng Pad->AveragePool->Reshape (khác 98 chỉ GAP)."""
        for _ in range(6):
            if tname not in producer:
                break
            n = producer[tname]
            if n.op_type in ('Reshape', 'Flatten', 'Squeeze', 'Unsqueeze',
                             'GlobalAveragePool', 'ReduceMean', 'AveragePool', 'Pad'):
                tname = n.input[0]
            else:
                break
        return tname

    feats = [trace_feature(c) for c in concat.input]
    print(f"==> Rewrite head: feats={feats} -> Conv1x1")

    # Weight Gemm fc.weight [196,176] -> Conv [196,176,1,1] (đúng thứ tự, không transpose)
    w = next(t for t in g.initializer if t.name == gemm.input[1])
    w_arr = numpy_helper.to_array(w)
    w_conv = numpy_helper.from_array(w_arr.reshape(w_arr.shape[0], w_arr.shape[1], 1, 1),
                                     name='head_w_conv')
    b = next(t for t in g.initializer if t.name == gemm.input[2])
    g.initializer.append(w_conv)

    # LƯU Ý QUAN TRỌNG (fix crash trên thiết bị):
    # 1) Conv PHẢI có dilations=[1,1] - esp-dl đọc attribute "dilations" trong
    #    Conv::get_output_shape, thiếu -> vector rỗng -> NULL deref (Guru Meditation).
    # 2) BỎ Reshape cuối (tránh lỗi shape không export được) -> output graph là
    #    tensor [1,196,1,1] - firmware chỉ cần đọc shape[1]==196.
    #
    # QUAN TRỌNG (fix lỗi "Conv fail: C: 130 kernel channels: 176"):
    # KHÔNG dùng ReduceMean KHÔNG axes! Theo ONNX spec, ReduceMean không axes
    # reduce TOÀN BỘ tensor (cả kênh C) -> [1,16,14,14] -> [1,1,1,1], mất 16 kênh,
    # làm Concat chỉ còn C=1+1+128=130 != 176 kênh kernel -> Conv fail.
    # Dùng GlobalAveragePool: chỉ reduce spatial H*W, GIỮ NGUYÊN kênh C
    # (đúng ngữ nghĩa AdaptiveAvgPool2d(1) của PyTorch gốc):
    #   GAP(getitem_120 [1,16,14,14]) -> [1,16,1,1] ✓
    #   GAP(relu_29     [1,32,7,7])   -> [1,32,1,1] ✓
    #   getitem_126 (conv8 output [1,128,1,1]) giữ nguyên ✓
    #   Concat -> [1,176,1,1] == kernel channels ✓
    t1, t2, cat, conv_out = 'head_t1', 'head_t2', 'head_cat', 'head_conv'
    new_nodes = [
        helper.make_node('GlobalAveragePool', [feats[0]], [t1]),
        helper.make_node('GlobalAveragePool', [feats[1]], [t2]),
        helper.make_node('Concat', [t1, t2, feats[2]], [cat], axis=1),
        # BẮT BUỘC khai báo đầy đủ attrs: esp-dl Conv::get_output_shape đọc
        # attribute 'dilations' - thiếu -> vector rỗng -> NULL deref
        # (Guru Meditation LoadProhibited). kernel_shape/strides/pads cũng cần
        # rõ ràng. Lỗi onnxsim 'rank (1) vs (4)' trước đây do OUTPUT shape rank 2
        # (đã fix bên dưới) + đã patch onnxsim.simplify -> không còn vướng.
        helper.make_node('Conv', [cat, w_conv.name, b.name], [conv_out],
                         kernel_shape=[1, 1], strides=[1, 1], pads=[0, 0, 0, 0],
                         dilations=[1, 1], group=1),
    ]
    # Graph output giờ là tensor conv_out [1,N,1,1] (không qua Reshape)
    if g.output[0].name != conv_out:
        g.output[0].name = conv_out
    # Output shape của Conv là [batch, N, 1, 1] (rank 4) - KHÔNG được để shape
    # cũ [batch, N] (rank 2) vì onnxsim báo 'Inferred shape differ in rank'.
    out_vi = g.output[0].type.tensor_type
    del out_vi.shape.dim[:]
    d0 = out_vi.shape.dim.add(); d0.dim_param = 'batch_size'
    d1 = out_vi.shape.dim.add(); d1.dim_value = landmark_size
    d2 = out_vi.shape.dim.add(); d2.dim_value = 1
    d3 = out_vi.shape.dim.add(); d3.dim_value = 1

    # Xoá toàn bộ node head cũ: mọi node backward-reachable từ Gemm nhưng
    # KHÔNG chạm backbone (dừng ở feats[0..2]). LƯU Ý model 68 torchlm/wgs có
    # NAME RỖNG -> xoá theo TENSOR, không theo node name.
    # 1) Tập node cần GIỮ: backbone (produce feats) + new_nodes.
    keep_out = set()
    # backbone: tất cả node produce feats hoặc tiền thân của chúng
    def collect_backbone(t):
        if t not in producer: return
        n = producer[t]
        if id(n) in keep_out: return
        keep_out.add(id(n))
        for i in n.input:
            collect_backbone(i)
    for f in feats:
        collect_backbone(f)
    # 2) Filter: giữ backbone + new_nodes, bỏ mọi thứ khác (head cũ + dead)
    filtered = [n for n in g.node if id(n) in keep_out] + new_nodes
    del g.node[:]
    g.node.extend(filtered)

    # Dọn initializer thừa (fc.weight cũ giờ không dùng) - giữ w_conv/bias mới
    used_init = set()
    for n in g.node:
        used_init.update(n.input)
    keep_init = [t for t in g.initializer if t.name in used_init]
    del g.initializer[:]
    g.initializer.extend(keep_init)

    # Dọn value_info thừa (shape khai báo của tensor đã bị xóa -> onnxsim
    # báo 'Inferred shape differ in rank'). Giữ value_info của tensor còn tồn tại.
    live_tensors = {i.name for i in g.input} | {t.name for t in g.initializer}
    for n in g.node:
        live_tensors.update(n.output)
    keep_vi = [vi for vi in g.value_info if vi.name in live_tensors]
    del g.value_info[:]
    g.value_info.extend(keep_vi)
    onnx.save(model, tmp_path)

    # --- SẮP XẾP TÔ-PÔ graph (model 68 torchlm/wgs có node nằm lộn xộn ->
    # onnx-simplifier báo 'Nodes must be topologically sorted') ---
    # Bảo toàn thứ tự ban đầu nhưng đẩy node chỉ khi input đã sẵn sàng.
    inputs_avail = {i.name for i in graph.input} | {t.name for t in g.initializer}
    ordered, pending = [], list(g.node)
    progress = True
    while pending and progress:
        progress = False
        still = []
        for n in pending:
            if all(i in inputs_avail for i in n.input):
                ordered.append(n)
                inputs_avail.update(n.output)
                progress = True
            else:
                still.append(n)
        pending = still
    if pending:
        # Node còn lại (không thể sắp xếp) -> giữ nguyên thứ tự cũ
        ordered.extend(pending)
    del g.node[:]
    g.node.extend(ordered)
    onnx.save(model, tmp_path)

    # --- c) Kiểm chứng nhanh bằng onnxruntime (KHÔNG chặn nếu model 68 có conv
    # channels 16/32 -> onnxruntime NCHWc 'ReorderInput' lỗi, nhưng esp-ppq vẫn
    # quantize được -> chỉ cảnh báo, không raise) ---
    try:
        import onnxruntime as ort
        import numpy as np
        so = ort.SessionOptions()
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        sess = ort.InferenceSession(tmp_path, providers=['CPUExecutionProvider'], sess_options=so)
        out = sess.run(None, {sess.get_inputs()[0].name: np.random.rand(1, 3, 112, 112).astype(np.float32)})[0]
        assert out.size == landmark_size, f"Output size sai sau rewrite: {out.shape} (cần {landmark_size})"
        print(f"==> ONNX sau tiền xử lý OK (output {out.shape}, {out.size} phần tử)")
    except Exception as e:
        print(f"[WARN] Bỏ qua verify onnxruntime (esp-ppq vẫn quantize được): {str(e)[:80]}")
    return tmp_path


# ----------------------------------------------------------------------------
# 2. Calibration dataset (preprocessing: /255 -> [0,1], ToTensor-style)
# ----------------------------------------------------------------------------
class FaceCropDataset(Dataset):
    """
    Đọc ẢNH KHUÔN MẶT từ thư mục (quét đệ quy), tự crop khuôn mặt lớn nhất
    bằng Haar cascade (khớp input thật của PFLD = face crop 112x112), chia 255.

    LƯU Ý: dataset chỉ có ảnh MẮT đơn lẻ (vd Kaggle drowsy-detection-dataset)
    KHÔNG dùng được - PFLD cần ảnh khuôn mặt đầy đủ để calibration đúng.
    """

    def __init__(self, img_dir: str, max_samples: int = 1000, face_crop: bool = True):
        import cv2

        self.cv2 = cv2
        self.face_crop = face_crop
        self.face_cascades = []
        if face_crop:
            # Dùng NHIỀU cascade (frontal + alt2 + profile) để bắt được cả ảnh
            # nghiêng đầu - Haar chuẩn rất nghiêm ngặt, hay bỏ sót ảnh góc.
            for name in ("haarcascade_frontalface_default.xml",
                         "haarcascade_frontalface_alt2.xml",
                         "haarcascade_profileface.xml"):
                c = cv2.CascadeClassifier(cv2.data.haarcascades + name)
                if not c.empty():
                    self.face_cascades.append(c)
        # Số ảnh DUY NHẤT không detect được mặt (set - không đếm lượt truy cập,
        # vì 1 ảnh được loader lấy lại nhiều lần trong calib_steps x batch)
        self.skipped = set()

        self.samples = []
        exts = (".jpg", ".jpeg", ".png", ".bmp")
        if img_dir and os.path.isdir(img_dir):
            # Quét ĐỆ QUY toàn bộ thư mục con (hỗ trợ cấu trúc dataset lồng nhau)
            # BỎ QUA thư mục 'discard/' — ảnh đã bị check_calib.py --filter loại
            for root, _, files in os.walk(img_dir):
                if os.path.basename(root) == "discard":
                    continue
                for fname in sorted(files):
                    if fname.lower().endswith(exts):
                        self.samples.append(os.path.join(root, fname))
                        if len(self.samples) >= max_samples:
                            break
                if len(self.samples) >= max_samples:
                    break
        if not self.samples:
            print("[WARN] Không tìm thấy ảnh trong calib_dir - dùng dữ liệu NGẪU NHIÊN "
                  "(chỉ test pipeline, model sẽ KHÔNG chính xác khi dùng thật!)")
            self.samples = None

    def __len__(self):
        return 500 if self.samples is None else len(self.samples)

    def __getitem__(self, idx):
        if self.samples is None:
            return torch.rand(3, 112, 112)  # random [0,1] (test pipeline)
        img = self.cv2.imread(self.samples[idx])
        if img is None:
            return torch.rand(3, 112, 112)

        # Crop khuôn mặt lớn nhất (giống input thật lúc inference)
        if self.face_cascades:
            h, w = img.shape[:2]
            # Ảnh lớn (video 1080p/4K): GIẢM KÍCH THƯỚC để Haar chạy nhanh
            # (detect trên ảnh nhỏ ~10x nhanh hơn, box scale ngược về toạ độ gốc)
            scale = 1.0
            if max(w, h) > 640:
                scale = 640.0 / max(w, h)
                detect_img = self.cv2.resize(img, (int(w * scale), int(h * scale)))
            else:
                detect_img = img
            gray = self.cv2.cvtColor(detect_img, self.cv2.COLOR_BGR2GRAY)
            all_faces = []
            for c in self.face_cascades:
                all_faces.extend(c.detectMultiScale(gray, 1.1, 5, minSize=(30, 30)))
            if len(all_faces) > 0:
                fx, fy, fw, fh = max(all_faces, key=lambda f: f[2] * f[3])
                if scale != 1.0:  # scale ngược về toạ độ ảnh gốc
                    fx, fy, fw, fh = (int(fx / scale), int(fy / scale),
                                      int(fw / scale), int(fh / scale))
                img = img[fy:fy + fh, fx:fx + fw]
            else:
                self.skipped.add(self.samples[idx])  # đếm ảnh DUY NHẤT

        img = self.cv2.cvtColor(img, self.cv2.COLOR_BGR2RGB)  # BGR -> RGB
        img = self.cv2.resize(img, (112, 112))
        img = img.astype(np.float32) / 255.0                  # [0,1]
        img = torch.from_numpy(img).permute(2, 0, 1)          # HWC -> CHW
        return img


def collate_fn(batch):
    # Stack batch thành 1 tensor [B, 3, 112, 112] - esp-ppq yêu cầu số tensor
    # khớp số input của graph (model PFLD có đúng 1 input).
    return torch.stack(batch)


def main():
    parser = argparse.ArgumentParser(description="Quantize PFLD ONNX -> .espdl (ESP-PPQ)")
    parser.add_argument("-i", "--input", required=True, help="PFLD ONNX (fp32)")
    parser.add_argument("-o", "--output", default="model/pfld_landmarks_98.espdl", help="Output .espdl")
    parser.add_argument("--calib-dir", default="",
                        help="Thư mục ảnh KHUÔN MẶT calibration (tạo thư mục này, vd: mkdir calib_data). "
                             "Dataset ảnh mắt đơn lẻ (Kaggle drowsy-detection) KHÔNG dùng được!")
    parser.add_argument("--no-face-crop", action="store_true",
                        help="Tắt tự động crop khuôn mặt (dùng toàn ảnh)")
    parser.add_argument("--calib-steps", type=int, default=128, help="Số bước calibration (mặc định 128)")
    parser.add_argument("--bits", type=int, default=8, help="8 hoặc 16 bit")
    parser.add_argument("--target", default="esp32s3", help="esp32s3 (mặc định)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        sys.exit(f"LỖI: không tìm thấy {args.input}. Chạy ./model/download_models.sh trước.")

    # ---- Tiền xử lý ONNX (cắt output phụ + sửa head Gemm) ----
    print(f"==> Load ONNX: {args.input}")
    onnx_for_quant = preprocess_onnx(args.input)

    print(f"==> Calibration: {args.calib_dir or '(không có thư mục - random, test only)'}, steps={args.calib_steps}")
    dataset = FaceCropDataset(args.calib_dir, face_crop=not args.no_face_crop)
    if dataset.samples:
        print(f"    Tìm thấy {len(dataset.samples)} ảnh (tự crop khuôn mặt: {'BẬT' if not args.no_face_crop else 'TẮT'})")
    dataloader = DataLoader(dataset=dataset, batch_size=16, shuffle=True, collate_fn=collate_fn)

    # Lưu ý: espdl_setting() của esp-ppq pip (>=1.0) không nhận tham số;
    # bit width được truyền qua num_of_bits của espdl_quantize_onnx bên dưới.
    setting = QuantizationSettingFactory.espdl_setting()

    print("==> Quantizing... (có thể mất vài phút trên CPU)")
    espdl_quantize_onnx(
        onnx_import_file=onnx_for_quant,
        espdl_export_file=args.output,
        calib_dataloader=dataloader,
        calib_steps=args.calib_steps,
        input_shape=[1, 3, 112, 112],
        target=args.target,
        num_of_bits=args.bits,
        setting=setting,
        device="cpu",
        error_report=False,
        export_test_values=True,  # cho phép model.test() trên thiết bị
        verbose=1,
    )

    if len(dataset.skipped) > 0:
        print(f"[WARN] {len(dataset.skipped)} ảnh (duy nhất) không detect được khuôn mặt "
              f"- đã dùng NGUYÊN KHUNG HÌNH cho calibration (chấp nhận được, "
              f"chỉ ảnh hưởng nhẹ). Muốn sạch: chạy tools/check_calib.py --filter calib_data")

    # Dọn file tạm
    if onnx_for_quant != args.input and os.path.exists(onnx_for_quant):
        os.remove(onnx_for_quant)

    size = os.path.getsize(args.output)
    print(f"==> OK: {args.output} ({size/1024/1024:.2f} MB)")
    print("==> Build firmware: idf.py build flash monitor")


if __name__ == "__main__":
    main()
