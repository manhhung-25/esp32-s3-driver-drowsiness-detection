#!/usr/bin/env python3
# ============================================================================
# annotate_landmark_video.py - Vẽ (annotate) landmark lên VIDEO trên host
# ----------------------------------------------------------------------------
# Giống verify_pfld_mapping.py nhưng xử lý VIDEO: mỗi frame, detect mặt
# (Haar cascade), chạy PFLD ONNX, vẽ N landmark + box mặt, rồi ghi ra video
# output. Giúp kiểm chứng mapping landmark qua nhiều frame/điều kiện ánh sáng.
#
# Hỗ trợ CẢ 2 model (giống verify_pfld_mapping.py):
#   * PFLD 98 điểm (WFLW):  mắt 60-75 (60-67 trái, 68-75 phải)
#   * PFLD 68 điểm (py-feat, iBUG/300-W): mắt 36-41/42-47, mũi 27-35, miệng 48-67
#
# Cách dùng (DÙNG ĐÚNG MÔI TRƯỜNG venv esp-ppq - KHÔNG dùng python/pip hệ thống):
#   ./tools/setup_quant_env.sh          # (1 lần) tạo ~/.venvs/esp-ppq + cài deps
#   ~/.venvs/esp-ppq/bin/python tools/annotate_landmark_video.py \
#       -m model/pfld68.onnx -i video.mp4 -o out.mp4 --max-frames 50
#
# Lưu ý: venv esp-ppq đã có sẵn onnx / onnxruntime / opencv-python<5 / numpy nên
# không cần cài thêm gì. (Chỉ cần cài lại nếu dùng môi trường khác.)
#
# Số frame demo thử trên video ngắn để kiểm tra mapping trước.
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


def detect_faces(gray, face_cascade, frame_idx):
    """Detect mặt (nhiều mặt) trả về list (x,y,w,h). Trả về toàn khung nếu detect fail."""
    try:
        faces = face_cascade.detectMultiScale(gray, 1.1, 5, minSize=(80, 80))
        return faces if len(faces) > 0 else []
    except (AttributeError, cv2.error):
        if frame_idx == 0:
            print("[WARN] cv2 không có CascadeClassifier (OpenCV 5?) -> dùng toàn khung.")
        return []


def run_pfld(sess, input_name, img, x0, y0, x1, y1):
    """Chạy PFLD trên vùng mặt (crop -> resize 112 -> normalize /255), trả pts [N,2]."""
    face = img[y0:y1, x0:x1]
    if face.size == 0:
        return None
    resized = cv2.resize(face, (112, 112))
    rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    blob = rgb.transpose(2, 0, 1)[None, ...].astype(np.float32)
    outputs = sess.run(None, {input_name: blob})
    out = next((o for o in outputs if len(o.shape) == 2 and o.shape[1] in (196, 136)), None)
    if out is None:
        return None
    npts = out.shape[1] // 2
    return out.reshape(npts, 2)


def main():
    ap = argparse.ArgumentParser(description="Annotate PFLD landmark lên video")
    ap.add_argument("-m", "--model", required=True, help="PFLD ONNX")
    ap.add_argument("-i", "--input", required=True, help="Video đầu vào")
    ap.add_argument("-o", "--output", default="result_landmarks.mp4", help="Video đầu ra (.mp4/.avi)")
    ap.add_argument("--max-frames", type=int, default=0, help="Chỉ xử lý N frame đầu (0 = tất cả)")
    ap.add_argument("--no-index", action="store_true", help="Không vẽ số index lên điểm")
    ap.add_argument("--fourcc", default="mp4v", help="Codec 4 ký tự (MJPEG/ mp4v/ XVID…)")
    args = ap.parse_args()

    try:
        sess = ort.InferenceSession(args.model, providers=["CPUExecutionProvider"])
    except Exception as e:
        sys.exit(f"Lỗi load model: {e}")
    input_name = sess.get_inputs()[0].name
    print(f"Input model: {sess.get_inputs()[0].shape} {sess.get_inputs()[0].type}")

    # ---- Mở video ----
    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        sys.exit(f"Không mở được video {args.input}")
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"Video: {width}x{height} @{fps:.2f}fps, {total} frames")

    # codec từ -o (mp4 --> mp4v ; avi --> MJPG) nếu --fourcc không rõ ràng
    fourcc = args.fourcc.lower()
    if fourcc in ("mp4v", "mp4", "m4v"):
        fourcc = "mp4v"
    elif fourcc in ("avi", "mjpg", "mjpeg"):
        fourcc = "MJPG"
    writer = cv2.VideoWriter(args.output, cv2.VideoWriter_fourcc(*fourcc), fps, (width, height))
    if not writer.isOpened():
        sys.exit("Lỗi mở VideoWriter - kiểm tra codec/đuôi file (/ -o).")

    # ---- Haar cascade (front face) ----
    face_cascade = None
    try:
        face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    except Exception:
        face_cascade = None
    if face_cascade is None or face_cascade.empty():
        print("[WARN] Thiếu haar cascade - mỗi frame dùng toàn khung (mắt slow).")

    frame_idx = 0
    while True:
        ok, img = cap.read()
        if not ok:
            break
        frame_idx += 1
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)

        h, w = img.shape[:2]
        faces = detect_faces(gray, face_cascade, frame_idx)
        if len(faces) == 0 and (face_cascade is None or face_cascade.empty()):
            faces = [(0, 0, w, h)]

        for (fx, fy, fw, fh) in faces:
            x0, y0, x1, y1 = fx, fy, fx + fw, fy + fh
            pts = run_pfld(sess, input_name, img, x0, y0, x1, y1)
            if pts is None:
                print(f"[frame {frame_idx}] PFLD không ra output -> bỏ khuôn mặt này")
                continue
            npts = pts.shape[0]

            cv2.rectangle(img, (x0, y0), (x1, y1), (0, 255, 0), 2)
            eye_idx, nose_idx, mouth_idx = get_landmark_indices(npts)
            for i, (nx, ny) in enumerate(pts):
                px = int(x0 + nx * (x1 - x0))
                py = int(y0 + ny * (y1 - y0))
                color = (0, 255, 0)
                if i in eye_idx:
                    color = (0, 0, 255)    # mắt: đỏ
                elif i in nose_idx:
                    color = (255, 0, 0)    # mũi: xanh dương
                elif i in mouth_idx:
                    color = (0, 255, 255)  # miệng: vàng
                cv2.circle(img, (px, py), 2, color, -1)
                if not args.no_index:
                    cv2.putText(img, str(i), (px + 2, py - 2),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.25, color, 1)

        writer.write(img)
        if frame_idx % 30 == 0:
            print(f"... {frame_idx}/{total} frames")
        if args.max_frames and frame_idx >= args.max_frames:
            break

    cap.release()
    writer.release()
    print(f"==> Đã lưu {args.output} ({frame_idx} frame).")

    if face_cascade is None or face_cascade.empty():
        print("Lưu ý: chưa detect mặt -> nếu video có nhiều người, mỗi face toàn khung; hãy cài OpenCV đầy đủ.")


if __name__ == "__main__":
    main()