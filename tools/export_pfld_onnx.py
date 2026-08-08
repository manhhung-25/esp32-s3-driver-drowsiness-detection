#!/usr/bin/env python3
# ============================================================================
# export_pfld_onnx.py - Export PFLD 98 landmark từ PyTorch checkpoint -> ONNX
# ----------------------------------------------------------------------------
# Architecture tái dựng CHÍNH XÁC từ source gốc của tác giả PFLD
# (guoqiangqi/PFLD model2.py - TensorFlow) sang PyTorch:
#   - MobileNetV2-style backbone, STRIDE NẰM TRÊN conv expand 1x1 (đặc trưng PFLD)
#   - Residual chỉ áp dụng từ block thứ 2 trở đi trong mỗi stage
#   - Head: concat(GAP(conv6_1=16) + GAP(conv7=32) + flatten(conv8=128)) = 176 -> FC 196
#
# Cách dùng:
#   # (tuỳ chọn) có checkpoint PyTorch (pytorch-pfld style):
#   python tools/export_pfld_onnx.py -c <checkpoint.pth> -o model/pfld.onnx
#
#   # không có checkpoint -> sinh ONNX trọng số ngẫu nhiên (chỉ để test pipeline):
#   python tools/export_pfld_onnx.py -o model/pfld.onnx
#
# Script tự kiểm chứng: chạy ONNX qua onnxruntime, bắt buộc output [1,196].
# ============================================================================

import argparse
import os
import sys

import numpy as np
import torch
import torch.nn as nn


# ----------------------------------------------------------------------------
# Các khối cơ bản (tái dựng từ guoqiangqi/PFLD model2.py)
# ----------------------------------------------------------------------------
class ConvBNReLU(nn.Module):
    """conv + bn + relu6 (slim dùng relu6; eps BN = 0.001 giống TF)."""

    def __init__(self, in_ch, out_ch, k, s, p, groups=1, relu=True, eps=1e-3):
        super().__init__()
        self.conv = nn.Conv2d(in_ch, out_ch, k, s, p, groups=groups, bias=False)
        self.bn = nn.BatchNorm2d(out_ch, eps=eps)
        self.relu = nn.ReLU6(inplace=True) if relu else nn.Identity()

    def forward(self, x):
        return self.relu(self.bn(self.conv(x)))


class PfldBlock(nn.Module):
    """MobileNetV2 block biến thể PFLD: expand(1x1, CÓ stride) -> dw(3x3, s1) -> linear(1x1)."""

    def __init__(self, inp, oup, exp, stride, residual=False):
        super().__init__()
        self.expand = ConvBNReLU(inp, exp, 1, stride, 0)   # stride nằm ở expand!
        self.dwise = ConvBNReLU(exp, exp, 3, 1, 1, groups=exp)
        self.linear = ConvBNReLU(exp, oup, 1, 1, 0, relu=False)
        self.residual = residual

    def forward(self, x):
        y = self.linear(self.dwise(self.expand(x)))
        return x + y if self.residual else y


class PFLDInference(nn.Module):
    """PFLD 98 landmark: input [1,3,112,112] -> output [1,196]."""

    def __init__(self):
        super().__init__()
        self.conv1 = ConvBNReLU(3, 64, 3, 2, 1)
        self.conv2 = ConvBNReLU(64, 64, 3, 1, 1, groups=64)  # depthwise thuần

        # stage 3 (28x28): 64ch
        self.conv3_1 = PfldBlock(64, 64, 128, 2, residual=False)
        self.conv3_2 = PfldBlock(64, 64, 128, 1, residual=True)
        self.conv3_3 = PfldBlock(64, 64, 128, 1, residual=True)
        self.conv3_4 = PfldBlock(64, 64, 128, 1, residual=True)
        self.conv3_5 = PfldBlock(64, 64, 128, 1, residual=True)

        # stage 4 (14x14): 128ch
        self.conv4_1 = PfldBlock(64, 128, 128, 2, residual=False)

        # stage 5 (14x14): 128ch (conv5_1 KHÔNG residual dù cùng shape - đúng source gốc)
        self.conv5_1 = PfldBlock(128, 128, 512, 1, residual=False)
        self.conv5_2 = PfldBlock(128, 128, 512, 1, residual=True)
        self.conv5_3 = PfldBlock(128, 128, 512, 1, residual=True)
        self.conv5_4 = PfldBlock(128, 128, 512, 1, residual=True)
        self.conv5_5 = PfldBlock(128, 128, 512, 1, residual=True)
        self.conv5_6 = PfldBlock(128, 128, 512, 1, residual=True)

        # stage 6: 16ch (feature nhỏ cho avg_pool1)
        self.conv6_1 = PfldBlock(128, 16, 256, 1, residual=False)

        # head
        self.conv7 = ConvBNReLU(16, 32, 3, 2, 1)          # 7x7x32 (avg_pool2)
        self.conv8 = ConvBNReLU(32, 128, 7, 1, 0, relu=False)  # 1x1x128, padding VALID
        self.avgpool = nn.AdaptiveAvgPool2d(1)
        self.fc = nn.Linear(16 + 32 + 128, 196)

    def forward(self, x):
        x = self.conv1(x)              # 56x56x64
        x = self.conv2(x)              # 56x56x64
        x = self.conv3_1(x)            # 28x28x64
        x = self.conv3_2(x)
        x = self.conv3_3(x)
        x = self.conv3_4(x)
        x = self.conv3_5(x)            # 28x28x64
        x = self.conv4_1(x)            # 14x14x128
        x = self.conv5_1(x)
        x = self.conv5_2(x)
        x = self.conv5_3(x)
        x = self.conv5_4(x)
        x = self.conv5_5(x)
        x = self.conv5_6(x)            # 14x14x128
        f16 = self.conv6_1(x)          # 14x14x16
        f32 = self.conv7(f16)          # 7x7x32
        f128 = self.conv8(f32)         # 1x1x128
        p1 = self.avgpool(f16).flatten(1)  # 16
        p2 = self.avgpool(f32).flatten(1)  # 32
        p3 = f128.flatten(1)               # 128
        return self.fc(torch.cat([p1, p2, p3], dim=1))  # 196


def main():
    parser = argparse.ArgumentParser(description="Export PFLD98 PyTorch -> ONNX")
    parser.add_argument("-c", "--checkpoint", default="", help="PyTorch checkpoint (.pth/.pt)")
    parser.add_argument("-o", "--output", default="model/pfld.onnx", help="Output ONNX")
    parser.add_argument("--input-size", type=int, default=112)
    args = parser.parse_args()

    model = PFLDInference()
    model.eval()

    if args.checkpoint:
        if not os.path.exists(args.checkpoint):
            sys.exit(f"LỖI: không tìm thấy checkpoint {args.checkpoint}")
        ckpt = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
        sd = ckpt.get("state_dict", ckpt) if isinstance(ckpt, dict) else ckpt
        # Load với strict=False + báo cáo độ khớp
        missing, unexpected = model.load_state_dict(sd, strict=False)
        print(f"  Keys khớp: {len(sd) - len(unexpected)} / {len(sd)}")
        print(f"  Missing:  {list(missing)[:5]}{'...' if len(missing) > 5 else ''}")
        print(f"  Unexpected (aux/net khác): {len(unexpected)}")
        if len(missing) > 0:
            print("[WARN] Checkpoint không khớp hoàn toàn architecture - kết quả có thể sai!")
            print("       Kiểm chứng bằng tools/verify_pfld_mapping.py trước khi dùng.")
    else:
        print("[WARN] Không có checkpoint -> ONNX trọng số NGẪU NHIÊN (chỉ test pipeline).")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
    dummy = torch.randn(1, 3, args.input_size, args.input_size)
    torch.onnx.export(
        model, dummy, args.output,
        input_names=["input"], output_names=["landmarks"],
        opset_version=13, do_constant_folding=True,
    )

    # ---- Tự kiểm chứng output shape [1, 196] ----
    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(args.output, providers=["CPUExecutionProvider"])
        out = sess.run(None, {sess.get_inputs()[0].name: dummy.numpy()})[0]
        assert out.shape == (1, 196), f"Output shape sai: {out.shape}"
        print(f"==> OK: {args.output} (output {out.shape})")
        print("==> Quantize: python tools/quantize_pfld.py -i model/pfld.onnx -o model/pfld_landmarks_98.espdl")
    except ImportError:
        print(f"==> Đã export {args.output} (cài onnxruntime để tự kiểm chứng shape).")


if __name__ == "__main__":
    main()
