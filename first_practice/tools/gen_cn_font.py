#!/usr/bin/env python3
"""Generate a minimal LVGL CJK font for the pose-assessment UI.
"""
import subprocess, sys, os

NEEDED = ("深蹲平板支撑站立前屈开始停止历史得分"
          "左膝右肘角度建议注意保持背部挺直"
          "收紧核心提示检测到超伸模式评测"
          "准备系统将为您实时提供纠正"
          "显示帧率分数建议")

OUT = os.path.normpath(os.path.join(os.path.dirname(__file__) or ".",
                       "..", "main", "font_cn.c"))

def gen(ttf_path, out_path):
    chars = "".join(sorted(set(NEEDED)))
    symbols = ",".join(f"0x{ord(c):04X}" for c in chars)
    cmd = ["lv_font_conv", "--font", ttf_path,
           "-r", "0x20-0x7f", "--symbols", symbols,
           "--size", "14", "--bpp", "4",
           "--format", "lvgl", "-o", out_path,
           "--force-fast-kern-format"]
    subprocess.run(cmd, check=True)
    print(f"Font generated: {out_path}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python tools/gen_cn_font.py <ttf>")
        print("Install lv_font_conv: npm install -g lv_font_conv")
        sys.exit(1)
    gen(sys.argv[1], OUT)
