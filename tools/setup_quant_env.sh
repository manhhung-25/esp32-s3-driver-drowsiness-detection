#!/usr/bin/env bash
# ============================================================================
# setup_quant_env.sh - Tạo môi trường Python ĐÚNG cho ESP-PPQ quantization
# ----------------------------------------------------------------------------
# VẤN ĐỀ: esp-ppq (1.3.6) chỉ hỗ trợ Python 3.8 - 3.12.
#         Máy bạn đang dùng Python 3.14 -> pip báo "No matching distribution".
#
# Script này:
#   1. Tự tìm Python 3.10/3.11/3.12 có sẵn trên máy
#   2. Nếu không có -> in hướng dẫn cài (apt / pyenv)
#   3. Tạo venv ~/.venvs/esp-ppq + cài esp-ppq, onnx, onnxruntime, torch...
#   4. In lệnh dùng venv để chạy quantize
#
# Cách dùng:
#   ./tools/setup_quant_env.sh
#   ~/.venvs/esp-ppq/bin/python tools/quantize_pfld.py -i model/pfld.onnx -o model/pfld_landmarks_98.espdl ...
# ============================================================================
set -e

VENV_DIR="${VENV_DIR:-$HOME/.venvs/esp-ppq}"

echo "==> Kiểm tra Python..."
# Tìm Python 3.10-3.12 (thứ tự ưu tiên)
PY="$HOME/.pyenv/versions/3.12.13/bin/python"

if [ ! -x "$PY" ]; then
    echo "Python 3.12 not found."
    exit 1
fi

if [ -z "$PY" ]; then
    echo ""
    echo "!! KHÔNG TÌM THẤY Python 3.10-3.12. Máy bạn chỉ có Python 3.14."
    echo ""
    echo "   Cách cài Python 3.12 (Ubuntu/Debian):"
    echo "     sudo apt update && sudo apt install -y python3.12 python3.12-venv"
    echo ""
    echo "   Hoặc dùng pyenv:"
    echo "     curl -fsSL https://pyenv.run | bash"
    echo "     pyenv install 3.12 && pyenv global 3.12"
    echo ""
    echo "   Rồi chạy lại script này."
    exit 1
fi

echo "==> Dùng $PY ($("$PY" --version))"
echo "==> Tạo venv: $VENV_DIR"
"$PY" -m venv "$VENV_DIR"

echo "==> Cài esp-ppq + onnx + onnxruntime + torch + opencv (vài phút, ~2GB)..."
"$VENV_DIR/bin/pip" install --upgrade pip
# opencv<5: OpenCV 5.0 đã GỠ BỎ Haar CascadeClassifier (script calibration cần dùng)
"$VENV_DIR/bin/pip" install esp-ppq onnx onnxruntime torch torchvision "opencv-python<5"

echo ""
echo "==> OK! Môi trường sẵn sàng. Chạy quantization bằng:"
echo "    $VENV_DIR/bin/python tools/quantize_pfld.py -i model/pfld.onnx -o model/pfld_landmarks_98.espdl \\"
echo "        --calib-dir calib_data --calib-steps 128"
echo ""
echo "    (Gợi ý: alias espdl='$VENV_DIR/bin/python' để gõ ngắn)"
