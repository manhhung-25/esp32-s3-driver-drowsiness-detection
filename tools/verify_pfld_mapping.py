#!/usr/bin/env python3
# ============================================================================
# verify_pfld_mapping.py - Kiểm chứng mapping landmark trên host
# ----------------------------------------------------------------------------
# Trước khi chạy trên thiết bị, kiểm tra model PFLD của bạn có đúng thứ tự
# điểm không. Hỗ trợ CẢ 2 model:
#   * PFLD 98 điểm (WFLW): output [1,196]
#       mắt 60-75 (60-67 trái, 68-75 phải), mũi 51-59, miệng 76-97
#   * PFLD 68 điểm (py-feat/pfld, iBUG/300-W): output [1,136]
#       mắt 36-41 (trái) / 42-47 (phải), mũi 27-35, miệng 48-67
#
# Cách dùng:
#   pip install onnx onnxruntime opencv-python numpy
#   python tools/verify_pfld_mapping.py -m model/pfld.onnx -i ảnh_có_mặt.jpg
#
# Output: ảnh result_landmarks.jpg vẽ N điểm kèm index (N = 98 hoặc 68).
#         Ảnh đúng khi mắt/mũi/miệng nằm đúng index theo mapping trên.
# ============================================================================

import argparse
import sys

import cv2
import numpy as np
import onnxruntime as ort

def get_landmark_indices(npts):
    """Trả về (eye, nose, mouth) theo số điểm (98 hoặc 68)."""
    if npts == 98:
        return (range(60, 76), range(51, 60), range(76, 98))
    if npts == 68:
        # py-feat/pfld: iBUG/300-W - mắt 36-41/42-47, mũi 27-35, miệng 48-67
        return (range(36, 48), range(27, 36), range(48, 68))
    return (range(60, 76), range(51, 60), range(76, 98))


def main():
    parser = argparse.ArgumentParser(description="Verify PFLD landmark mapping (98 hoặc 68 điểm)")
    parser.add_argument("-m", "--model", required=True, help="PFLD ONNX")
    parser.add_argument("-i", "--image", required=True, help="Ảnh có khuôn mặt")
    parser.add_argument("-o", "--output", default="result_landmarks.jpg")
    args = parser.parse_args()

    sess = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    input_name = sess.get_inputs()[0].name
    print(f"Input: {sess.get_inputs()[0].shape} {sess.get_inputs()[0].type}")

    img = cv2.imread(args.image)
    if img is None:
        raise SystemExit(f"Không đọc được ảnh {args.image}")
    h, w = img.shape[:2]

    # ---- Detect mặt bằng Haar cascade (chỉ để crop - có thể thay bằng ảnh crop sẵn) ----
    # LƯU Ý: OpenCV 5.0 đã gỡ CascadeClassifier -> bọc try/except
    x0, y0, x1, y1 = 0, 0, w, h
    try:
        face_cascade = cv2.CascadeClassifier(
            cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
        faces = face_cascade.detectMultiScale(img, 1.1, 5, minSize=(80, 80))
        if len(faces) == 0:
            print("[WARN] Không detect được mặt - dùng toàn ảnh. Tốt nhất nên crop sẵn khuôn mặt.")
        else:
            (fx, fy, fw, fh) = faces[0]
            x0, y0, x1, y1 = fx, fy, fx + fw, fy + fh
    except (AttributeError, cv2.error):
        print("[WARN] cv2 không có CascadeClassifier (OpenCV 5?) - dùng toàn ảnh.")

    # ---- Crop + resize 112x112 + normalize (ToTensor-style: chỉ /255, input [0,1]) ----
    # Khớp với preprocessing trong firmware (Kconfig DROWSY_PFLD_MEAN=0/STD=255)
    face = img[y0:y1, x0:x1]
    resized = cv2.resize(face, (112, 112))
    rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    blob = rgb.transpose(2, 0, 1)[None, ...].astype(np.float32)

    outputs = sess.run(None, {input_name: blob})
    # Model có thể có output phụ (feature map) -> chọn output landmark 2D [1,196] hoặc [1,136]
    out = next((o for o in outputs if len(o.shape) == 2 and o.shape[1] in (196, 136)), None)
    if out is None:
        sys.exit("LỖI: không tìm thấy output landmark [1,196] (98 điểm) hoặc [1,136] (68 điểm) trong model")
    npts = out.shape[1] // 2
    pts = out.reshape(npts, 2)
    print(f"Output shape: {out.shape} -> {npts} điểm")

    # ---- Vẽ lên ảnh gốc ----
    eye_idx, nose_idx, mouth_idx = get_landmark_indices(npts)
    vis = img.copy()
    cv2.rectangle(vis, (x0, y0), (x1, y1), (0, 255, 0), 2)
    for i, (nx, ny) in enumerate(pts):
        px = int(x0 + nx * (x1 - x0))
        py = int(y0 + ny * (y1 - y0))
        color = (0, 255, 0)
        if i in eye_idx:
            color = (0, 0, 255)   # mắt: đỏ
        elif i in nose_idx:
            color = (255, 0, 0)   # mũi: xanh dương
        elif i in mouth_idx:
            color = (0, 255, 255) # miệng: vàng
        cv2.circle(vis, (px, py), 2, color, -1)
        cv2.putText(vis, str(i), (px + 2, py - 2), cv2.FONT_HERSHEY_SIMPLEX, 0.25, color, 1)

    cv2.imwrite(args.output, vis)
    print(f"==> Đã lưu {args.output}")

    if npts == 98:
        print("Kiểm tra: mắt (đỏ) 60-75, mũi (xanh) 51-59, miệng (vàng) 76-97.")
        print("Nếu khớp -> mapping trong landmark_pfld.cpp đúng. Nếu lệch -> chỉnh hằng số.")
    else:
        print(f"Kiểm tra (iBUG-68): mắt (đỏ) 36-47, mũi (xanh) 27-35, miệng (vàng) 48-67.")
        print("Nếu khớp -> mapping trong landmark_pfld.cpp đúng (EAR dùng 36-41/42-47).")


if __name__ == "__main__":
    main()
