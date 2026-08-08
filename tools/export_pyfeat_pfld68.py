#!/usr/bin/env python3
"""
Export PFLD 68 điểm từ checkpoint py-feat/pfld (HF) sang ONNX.

Checkpoint: pfld_model_best.pth.tar (fc.weight (136,176) = 68 điểm)
Kiến trúc: PFLD chuẩn (khớp cunjian/pfld_compressed):
  conv1[3,64,3,2] -> dw_pool -> conv1_extra -> InvertedResidual blocks
  -> conv6_1(16) -> concat(avg_pool1(16) + avg_pool2(32) + conv8(128)) = 176
  -> fc(176, 136)
Input [1,3,112,112] RGB (normalize /255) -> Output [1,136]

Cách dùng:
  ~/.venvs/esp-ppq/bin/python tools/export_pyfeat_pfld68.py \
      -c /tmp/pfld_model_best.pth.tar -o model/pfld68_pyfeat.onnx
"""
import argparse
import torch
import torch.nn as nn


class ConvBN(nn.Module):
    def __init__(self, inp, oup, k, s, p, groups=1, relu=True, eps=1e-3):
        super().__init__()
        self.conv = nn.Conv2d(inp, oup, k, s, p, groups=groups, bias=False)
        self.bn = nn.BatchNorm2d(oup, eps=eps)
        self.relu = nn.ReLU(inplace=True) if relu else nn.Identity()

    def forward(self, x):
        return self.relu(self.bn(self.conv(x)))


class InvertedResidual(nn.Module):
    def __init__(self, inp, oup, stride, use_res, expand):
        super().__init__()
        # conv.0 expand(1x1, stride) -> conv.3 dwise(3x3, s1) -> conv.6 linear(1x1)
        self.conv = nn.Sequential(
            ConvBN(inp, inp * expand, 1, stride, 0),
            ConvBN(inp * expand, inp * expand, 3, 1, 1, groups=inp * expand),
            ConvBN(inp * expand, oup, 1, 1, 0, relu=False),
        )
        self.use_res = use_res

    def forward(self, x):
        y = self.conv(x)
        return x + y if self.use_res else y


class PFLDInference(nn.Module):
    """PFLD 68 điểm — khớp checkpoint py-feat/pfld."""

    def __init__(self):
        super().__init__()
        self.conv1 = ConvBN(3, 64, 3, 2, 1)
        # depth-wise pooling: dw_pool(dwise+BN) + conv1_extra(1x1 KHÔNG BN) + ReLU
        # (checkpoint: dw_pool.weight, dw_bn.*, conv1_extra.weight - không có BN extra)
        self.dw_pool = nn.Sequential(
            ConvBN(64, 64, 3, 1, 1, groups=64),                 # dw_pool + dw_bn
            nn.Conv2d(64, 64, 1, 1, 0, bias=False),              # conv1_extra (không BN)
            nn.ReLU(inplace=True),
        )
        # stage 3 (64ch): conv3_1 (không residual, stride 2) + block3_2..3_5 (residual)
        self.conv3_1 = InvertedResidual(64, 64, 2, False, 1)
        self.block3_2 = InvertedResidual(64, 64, 1, True, 1)
        self.block3_3 = InvertedResidual(64, 64, 1, True, 1)
        self.block3_4 = InvertedResidual(64, 64, 1, True, 1)
        self.block3_5 = InvertedResidual(64, 64, 1, True, 1)
        # stage 4 (128ch)
        self.conv4_1 = InvertedResidual(64, 128, 2, False, 1)
        # stage 5 (128ch, expand 2)
        self.conv5_1 = InvertedResidual(128, 128, 1, False, 2)
        self.block5_2 = InvertedResidual(128, 128, 1, True, 2)
        self.block5_3 = InvertedResidual(128, 128, 1, True, 2)
        self.block5_4 = InvertedResidual(128, 128, 1, True, 2)
        self.block5_5 = InvertedResidual(128, 128, 1, True, 2)
        self.block5_6 = InvertedResidual(128, 128, 1, True, 2)
        # head
        self.conv6_1 = InvertedResidual(128, 16, 1, False, 1)   # -> avg_pool1 (16)
        self.conv7 = ConvBN(16, 32, 3, 2, 1)   # ConvBN trực tiếp (không Sequential)
        self.conv8 = ConvBN(32, 128, 7, 1, 0, relu=False)       # 1x1x128 (padding 0)
        self.avg_pool1 = nn.AdaptiveAvgPool2d(1)
        self.avg_pool2 = nn.AdaptiveAvgPool2d(1)
        self.fc = nn.Linear(176, 136)

    def forward(self, x):
        x = self.conv1(x)                    # 56
        x = self.dw_pool(x)                  # 56
        x = self.conv3_1(x)                  # 28
        x = self.block3_2(x)
        x = self.block3_3(x)
        x = self.block3_4(x)
        x = self.block3_5(x)                 # 28 (64)
        x = self.conv4_1(x)                  # 14 (128)
        x = self.conv5_1(x)                  # 14
        x = self.block5_2(x)
        x = self.block5_3(x)
        x = self.block5_4(x)
        x = self.block5_5(x)
        x = self.block5_6(x)                 # 14 (128)
        f16 = self.conv6_1(x)                # 14 (16)
        f32 = self.conv7(f16)                # 7 (32)
        f128 = self.conv8(f32)               # 1 (128)
        p1 = self.avg_pool1(f16).flatten(1)  # 16
        p2 = self.avg_pool2(f32).flatten(1)  # 32
        p3 = f128.flatten(1)                 # 128
        return self.fc(torch.cat([p1, p2, p3], dim=1))  # 136


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--checkpoint", required=True)
    parser.add_argument("-o", "--output", default="model/pfld68_pyfeat.onnx")
    args = parser.parse_args()

    model = PFLDInference()
    ckpt = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    raw = ckpt.get("state_dict", ckpt)

    # Checkpoint dùng KEY PHẲNG (conv1.weight, conv3_1.conv.0.weight...)
    # trong khi module của ta lồng (conv1.conv.weight, conv3_1.conv.0.conv.weight).
    # -> Map key phẳng -> key lồng rồi load (strict).
    def flat_to_nested(flat_key):
        parts = flat_key.split(".")
        # InvertedResidual: checkpoint conv.{0,1,2,3,4,6,7}
        #   0=expand conv, 1=BN(expand), 3=dwise, 4=BN(dwise), 6=linear, 7=BN(linear)
        # module ta: conv.{0,1,2} = ConvBN(expand, dwise, linear)
        if len(parts) >= 3 and parts[1] == "conv" and parts[2].isdigit():
            sub = int(parts[2]); rest = ".".join(parts[3:])
            if sub == 0: return f"{parts[0]}.conv.0.conv.{rest}"
            if sub == 1: return f"{parts[0]}.conv.0.bn.{rest}"
            if sub == 3: return f"{parts[0]}.conv.1.conv.{rest}"
            if sub == 4: return f"{parts[0]}.conv.1.bn.{rest}"
            if sub == 6: return f"{parts[0]}.conv.2.conv.{rest}"
            if sub == 7: return f"{parts[0]}.conv.2.bn.{rest}"
        # conv1 / bn1 / dw_pool / dw_bn / conv1_extra / conv7 / conv8 / bn8 / fc
        if parts[0] == "conv1" and len(parts) == 2:
            if parts[1] == "weight": return "conv1.conv.weight"
            if parts[1] == "bias": return "conv1.bn.bias"
            if parts[1] == "running_mean": return "conv1.bn.running_mean"
            if parts[1] == "running_var": return "conv1.bn.running_var"
        if parts[0] == "bn1" and len(parts) == 2:
            return f"conv1.bn.{parts[1]}"
        # dw_pool(depthwise) + dw_bn(BN depthwise) + conv1_extra(1x1 linear)
        if parts[0] == "dw_pool" and len(parts) == 2:
            return f"dw_pool.0.conv.{parts[1]}" if parts[1] == "weight" else f"dw_pool.0.bn.{parts[1]}"
        if parts[0] == "dw_bn" and len(parts) == 2:
            return f"dw_pool.0.bn.{parts[1]}"
        if parts[0] == "conv1_extra" and len(parts) == 2:
            return f"dw_pool.1.{parts[1]}"   # Conv2d thuần (không BN)
        # conv7: checkpoint conv7.0(Conv) + conv7.1(BN) -> conv7.conv / conv7.bn
        # parts=['conv7','0'|'1','weight'|'bias'|'running_*'|'num_batches_tracked']
        if parts[0] == "conv7" and len(parts) == 3 and parts[2] != "num_batches_tracked":
            if parts[1] == "0": return "conv7.conv.weight"
            if parts[1] == "1": return f"conv7.bn.{parts[2]}"
        # conv8: Conv + BN riêng (conv8.weight=conv, conv8.bias=bn.bias, bn8=bn)
        if parts[0] == "conv8" and len(parts) == 2:
            if parts[1] == "weight": return "conv8.conv.weight"
            if parts[1] == "bias": return "conv8.bn.bias"
            if parts[1] == "running_mean": return "conv8.bn.running_mean"
            if parts[1] == "running_var": return "conv8.bn.running_var"
        if parts[0] == "bn8" and len(parts) == 2:
            return f"conv8.bn.{parts[1]}"
        return flat_key

    mapped = {flat_to_nested(k): v for k, v in raw.items()
              if not k.endswith("num_batches_tracked")}  # bỏ key không cần cho inference
    model.load_state_dict(mapped, strict=True)
    model.eval()

    dummy = torch.randn(1, 3, 112, 112)
    torch.onnx.export(model, dummy, args.output,
                      input_names=["input"], output_names=["output"],
                      opset_version=13, do_constant_folding=True)
    print(f"==> Exported {args.output}")

    # Verify
    import onnx
    m = onnx.load(args.output)
    print("Input :", [(i.name, [d.dim_value for d in i.type.tensor_type.shape.dim]) for i in m.graph.input])
    print("Output:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in m.graph.output])


if __name__ == "__main__":
    main()
