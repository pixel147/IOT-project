"""
Re-quantize PoseNet MobileNet FP32 → ESP-DL INT8
Calibration data: pixels normalized to [0, 1] (divided by 255)
"""
import os, sys
import numpy as np
from PIL import Image
import torch
from torch.utils.data import DataLoader, Dataset

ONNX_MODEL = "D:/Code_Projects/IOT/first_practice/posenet_mobilenet_fp32.onnx"
ESPDL_OUTPUT = "D:/Code_Projects/IOT/first_practice/posenet_mobilenet_v2.espdl"
CALIB_DIR = "D:/Code_Projects/MobileNET/calib_yolo11n-pose"
TARGET = "esp32p4"
INPUT_SHAPE = [1, 3, 513, 257]
NUM_CALIB = 32


class CalibDataset(Dataset):
    def __init__(self, data):
        self.data = data
    def __len__(self):
        return len(self.data)
    def __getitem__(self, idx):
        return torch.from_numpy(self.data[idx])


def main():
    files = sorted([f for f in os.listdir(CALIB_DIR) if f.endswith('.jpg')])[:NUM_CALIB]
    images = []
    for f in files:
        img = Image.open(os.path.join(CALIB_DIR, f)).convert('RGB')
        img = img.resize((257, 513), Image.BILINEAR)
        arr = np.array(img, dtype=np.float32)
        arr = arr / 255.0  # ← 归一化到 [0, 1]
        arr = arr.transpose(2, 0, 1)  # HWC → CHW
        images.append(arr)
    print(f"Loaded {len(images)} calibration images (normalized [0,1]), shape: {images[0].shape}")

    dataset = CalibDataset(images)
    dataloader = DataLoader(dataset, batch_size=1, shuffle=False)

    print(f"\nRe-quantizing (correct normalized range [0, 1])...")

    from esp_ppq.api import espdl_quantize_onnx

    espdl_quantize_onnx(
        onnx_import_file=ONNX_MODEL,
        espdl_export_file=ESPDL_OUTPUT,
        calib_dataloader=dataloader,
        calib_steps=NUM_CALIB,
        input_shape=INPUT_SHAPE,
        target=TARGET,
        num_of_bits=8,
        device='cpu',
        verbose=1,
    )

    size_kb = os.path.getsize(ESPDL_OUTPUT) / 1024
    print(f"\nDONE: {ESPDL_OUTPUT} ({size_kb:.0f} KB)")


if __name__ == "__main__":
    main()
