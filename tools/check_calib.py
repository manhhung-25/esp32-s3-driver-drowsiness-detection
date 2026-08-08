#!/usr/bin/env python3
# ============================================================================
# check_calib.py - Kiểm tra + LỌC bộ ảnh calibration
# ----------------------------------------------------------------------------
# Chế độ kiểm tra (mặc định): báo cáo bao nhiêu ảnh có khuôn mặt, độ sáng,
# độ nét, kích thước mặt... -> trả lời "bộ ảnh dùng được không?"
#
# Chế độ LỌC (--filter): TỰ ĐỘNG chuyển ảnh xấu vào thư mục discard/ để
# giữ lại đúng bộ ảnh tốt cho quantization. Tiêu chí giữ lại:
#   + Đọc được file
#   + CÓ khuôn mặt (bỏ --keep-noface để giữ cả ảnh không detect được mặt)
#   + Không quá tối  (mean >= --dark-thr, mặc định 10/255)
#   + Không bị mờ    (Laplacian variance >= --blur-thr, chỉ áp cho ảnh lớn)
#
# Cách dùng:
#   # Kiểm tra:        ~/.venvs/esp-ppq/bin/python tools/check_calib.py [thư_mục]
#   # Kiểm tra + lọc:  ~/.venvs/esp-ppq/bin/python tools/check_calib.py --filter [thư_mục]
#
# Yêu cầu: opencv-python<5 (Haar CascadeClassifier đã bị gỡ ở OpenCV 5)
# ============================================================================

import argparse
import os
import shutil
import sys

EXTS = (".jpg", ".jpeg", ".png", ".bmp")


def main():
    parser = argparse.ArgumentParser(description="Kiểm tra + lọc bộ ảnh calibration")
    parser.add_argument("dir", nargs="?", default="calib_data",
                        help="Thư mục ảnh (mặc định: calib_data/)")
    parser.add_argument("--filter", action="store_true",
                        help="LỌC: chuyển ảnh xấu (không có mặt/tối/mờ) sang thư mục discard/")
    parser.add_argument("--blur-thr", type=float, default=5.0,
                        help="Ngưỡng độ nét Laplacian (dưới ngưỡng = mờ, chỉ áp ảnh >= 300px)")
    parser.add_argument("--dark-thr", type=float, default=10.0,
                        help="Ngưỡng tối (mean < ngưỡng = ảnh đen/hỏng)")
    parser.add_argument("--keep-noface", action="store_true",
                        help="Giữ cả ảnh không detect được khuôn mặt (góc nghiêng...)")
    parser.add_argument("--discard-dir", default="discard",
                        help="Tên thư mục chứa ảnh bị loại (mặc định: <dir>/discard/)")
    args = parser.parse_args()

    if not os.path.isdir(args.dir):
        sys.exit(f"LỖI: thư mục '{args.dir}' không tồn tại.\n"
                 f"  Tạo và bỏ ảnh vào: mkdir -p {args.dir}")

    import cv2
    import numpy as np

    try:
        # Nhiều cascade (frontal + alt2 + profile) để bắt cả ảnh nghiêng đầu
        cascades = []
        for name in ("haarcascade_frontalface_default.xml",
                     "haarcascade_frontalface_alt2.xml",
                     "haarcascade_profileface.xml"):
            c = cv2.CascadeClassifier(cv2.data.haarcascades + name)
            if not c.empty():
                cascades.append(c)
        if not cascades:
            sys.exit("LỖI: không load được Haar cascade (opencv? cài opencv-python<5)")
    except (AttributeError, cv2.error):
        sys.exit("LỖI: cv2 không có CascadeClassifier - OpenCV 5? Cài opencv-python<5")

    # Quét đệ quy
    images = []
    for root, _, files in os.walk(args.dir):
        # bỏ qua thư mục discard cũ nếu có
        if os.path.basename(root) == args.discard_dir:
            continue
        for f in sorted(files):
            if f.lower().endswith(EXTS):
                images.append(os.path.join(root, f))

    if not images:
        sys.exit(f"Không có ảnh nào trong '{args.dir}'.")

    # ---- Thư mục discard (chỉ tạo khi lọc) ----
    discard_path = os.path.join(args.dir, args.discard_dir)
    if args.filter:
        os.makedirs(discard_path, exist_ok=True)

    print(f"==> Tổng ảnh: {len(images)}" + ("   (chế độ LỌC)" if args.filter else ""))

    with_face = 0
    face_sizes = []
    brightness = []
    blur_values = []
    no_face = []
    moved = {"noface": 0, "dark": 0, "blur": 0, "unreadable": 0}

    for path in images:
        img = cv2.imread(path)
        if img is None:
            if args.filter:
                _move(path, discard_path, moved, "unreadable")
            else:
                no_face.append(os.path.basename(path) + " (không đọc được)")
            continue

        h, w = img.shape[:2]
        bright = img.mean()
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        brightness.append(bright)

        # Độ nét (chỉ tính cho ảnh đủ lớn - ảnh nhỏ crop sẵn không dùng được Laplacian)
        blur = -1.0
        if max(w, h) >= 300:
            blur = cv2.Laplacian(gray, cv2.CV_64F).var()
            blur_values.append(blur)

        # Face detection (thu nhỏ ảnh lớn để chạy nhanh)
        scale = 1.0
        if max(w, h) > 640:
            scale = 640.0 / max(w, h)
            d_img = cv2.resize(img, (int(w * scale), int(h * scale)))
        else:
            d_img = img
        d_gray = cv2.cvtColor(d_img, cv2.COLOR_BGR2GRAY)
        all_faces = []
        for c in cascades:
            all_faces.extend(c.detectMultiScale(d_gray, 1.1, 5, minSize=(30, 30)))
        has_face = len(all_faces) > 0

        # ---- Chế độ LỌC: quyết định giữ / chuyển discard ----
        if args.filter:
            reason = None
            if bright < args.dark_thr:
                reason = "dark"
            elif not has_face and not args.keep_noface:
                reason = "noface"
            elif blur >= 0 and blur < args.blur_thr:
                reason = "blur"
            if reason is not None:
                _move(path, discard_path, moved, reason)
                continue

        # ---- Thống kê ảnh GIỮ LẠI ----
        if has_face:
            with_face += 1
            fx, fy, fw, fh = max(all_faces, key=lambda f: f[2] * f[3])
            if scale != 1.0:
                fx, fy, fw, fh = (int(fx / scale), int(fy / scale),
                                  int(fw / scale), int(fh / scale))
            face_sizes.append(min(fw, fh))
        else:
            no_face.append(os.path.basename(path))

    total_kept = len(images) - sum(moved.values())
    pct = 100.0 * with_face / total_kept if total_kept else 0
    avg_face = (sum(face_sizes) / len(face_sizes)) if face_sizes else 0
    min_face = min(face_sizes) if face_sizes else 0
    mean_bright = sum(brightness) / len(brightness) if brightness else 0
    mean_blur = sum(blur_values) / len(blur_values) if blur_values else -1

    print(f"==> Ảnh CÓ khuôn mặt: {with_face}/{total_kept} ({pct:.0f}%)")
    print(f"==> Kích thước khuôn mặt: trung bình {avg_face:.0f}px, nhỏ nhất {min_face}px "
          f"(khuyên >= 80px)")
    print(f"==> Độ sáng trung bình: {mean_bright:.1f}/255 (ảnh thật thường 40-200; "
          f"< 10 = ảnh ĐEN/dataset hỏng!)")
    if mean_blur >= 0:
        print(f"==> Độ nét trung bình (Laplacian): {mean_blur:.1f} (dưới {args.blur_thr:.0f} = mờ)")

    if args.filter:
        n_moved = sum(moved.values())
        print(f"==> LỌC: giữ {total_kept} ảnh, chuyển {n_moved} ảnh xấu vào "
              f"'{args.discard_dir}/' "
              f"({moved['noface']} không mặt, {moved['blur']} mờ, "
              f"{moved['dark']} tối, {moved['unreadable']} không đọc được)")
        print("    -> Kiểm tra lại thư mục discard/ trước khi xoá hẳn.")

    if mean_bright < 10:
        print("\nKẾT LUẬN: ❌ ẢNH BỊ ĐEN (dataset hỏng/giải nén sai) - KHÔNG dùng được!")
        return
    if no_face and not args.filter:
        print(f"==> {len(no_face)} ảnh KHÔNG có khuôn mặt (ảnh mắt đơn lẻ/ảnh sai?):")
        for name in no_face[:10]:
            print(f"    - {name}")
        if len(no_face) > 10:
            print(f"    ... và {len(no_face) - 10} ảnh nữa")

    # Kết luận
    print()
    small = all(
        (cv2.imread(p) is not None and cv2.imread(p).shape[1] <= 160 and cv2.imread(p).shape[0] <= 160)
        for p in images[:20]
    ) if images else False
    if pct == 0 and small:
        print("KẾT LUẬN: ✅ Ảnh NHỎ (đã crop sẵn khuôn mặt, vd WFLW 112x112) - DÙNG ĐƯỢC.")
        print("           Khi quantize nhớ thêm: --no-face-crop")
    elif pct >= 90:
        if avg_face >= 80:
            print("KẾT LUẬN: ✅ BỘ ẢNH DÙNG ĐƯỢC - tiến hành quantization.")
        else:
            print("KẾT LUẬN: ⚠️ Có khuôn mặt nhưng hơi nhỏ - vẫn dùng được, "
                  "tốt hơn nên crop gần hơn.")
    elif pct >= 80:
        print("KẾT LUẬN: ✅ Đa số ảnh có khuôn mặt - DÙNG ĐƯỢC (số ít còn lại thường là "
              "góc nghiêng - vẫn dùng nguyên khung hình, không sao).")
    elif pct >= 50:
        print("KẾT LUẬN: ⚠️ Gần nửa ảnh không có khuôn mặt - nên lọc bỏ ảnh sai "
              "(ảnh mắt đơn lẻ từ Kaggle drowsy-detection KHÔNG dùng được).")
    else:
        print("KẾT LUẬN: ❌ BỘ ẢNH KHÔNG PHÙ HỢP - cần ảnh KHUÔN MẶT ĐẦY ĐỦ "
              "(mắt + mũi + miệng), không phải ảnh mắt đơn lẻ.")


def _move(path, discard_dir, moved, reason):
    """Chuyển ảnh xấu vào thư mục discard (không xoá, tránh trùng tên)."""
    name = os.path.basename(path)
    dst = os.path.join(discard_dir, name)
    i = 1
    while os.path.exists(dst):
        stem, ext = os.path.splitext(name)
        dst = os.path.join(discard_dir, f"{stem}_{i}{ext}")
        i += 1
    try:
        shutil.move(path, dst)
        moved[reason] = moved.get(reason, 0) + 1
    except OSError:
        pass


if __name__ == "__main__":
    main()
