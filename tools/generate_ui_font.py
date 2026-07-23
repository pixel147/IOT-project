from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FONT_PATH = Path(r"C:\Windows\Fonts\Deng.ttf")
OUTPUT_C = Path(__file__).parents[1] / "main" / "ui_font_zh_22.c"
OUTPUT_H = Path(__file__).parents[1] / "main" / "ui_font_zh_22.h"
FONT_SIZE = 22

# 字库 — 情绪守护 + 聊天项目专用，删除旧姿态检测无关字符。
TEXT = (
    "情绪守护生气厌恶害怕开心难过惊讶平静脸检测置信度"
    "摄像头启动中实时状态等待模型接入开始停止提示已连接失败"
    "未检测到系统就绪设置"
    "建议专注深呼吸放松保持微笑你可以的请靠近吧太远啦"
    "关闭弹窗按钮确定取消加载网络错误成功切换模式版本"
    "图例心情统计今日周趋势对话记录无"
    "训练次数分钟"
    "享受此刻宁静想点开心的事别怕你很安全保持好心情"
    "试着放宽心吧哇真惊喜已经暂停开启监控"
    "余元公冷别制剩受叠右号名呢哇回填士大子宁安容宽"
    "射小局居布很息想所排控文映暂有机标栏此滚点用监"
    "相真着竖章素线自色行览角试调贴长间隔面预题"
    "满靠近编编号一上下主位侧内占左底据板由距部顶徽称"
    "，。：；（）-+/%~—"
)
ASCII = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"


def c_array(values, per_line=16):
    lines = []
    for offset in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(value) for value in values[offset:offset + per_line]) + ",")
    return "\n".join(lines)


def render_glyph(font, char):
    left, top, right, bottom = font.getbbox(char, anchor="ls")
    width = max(1, right - left)
    height = max(1, bottom - top)
    image = Image.new("L", (width, height), 0)
    ImageDraw.Draw(image).text((-left, -top), char, font=font, fill=255, anchor="ls")

    pixels = list(image.getdata())
    bitmap = []
    for index in range(0, len(pixels), 2):
        high = min(15, (pixels[index] + 8) // 17)
        low = min(15, (pixels[index + 1] + 8) // 17) if index + 1 < len(pixels) else 0
        bitmap.append((high << 4) | low)

    advance = max(1, round(font.getlength(char) * 16))
    return bitmap, advance, width, height, left, -bottom


def main():
    font = ImageFont.truetype(str(FONT_PATH), FONT_SIZE)
    ascent, descent = font.getmetrics()
    chars = sorted(set(TEXT + ASCII), key=ord)
    bitmap = []
    descriptors = [(0, 0, 0, 0, 0, 0)]

    for char in chars:
        glyph, advance, width, height, ofs_x, ofs_y = render_glyph(font, char)
        descriptors.append((len(bitmap), advance, width, height, ofs_x, ofs_y))
        bitmap.extend(glyph)

    glyphs = "\n".join(
        "    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}," % item
        for item in descriptors
    )
    cmaps = "\n".join(
        "    {.range_start = %d, .range_length = 1, .glyph_id_start = %d, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},"
        % (ord(char), index)
        for index, char in enumerate(chars, 1)
    )

    source = f'''#include "ui_font_zh_22.h"

/* 此文件由 tools/generate_ui_font.py 生成，请勿手工修改。 */
static const uint8_t glyph_bitmap[] = {{
{c_array(bitmap)}
}};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{glyphs}
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
{cmaps}
}};

static lv_font_fmt_txt_dsc_t font_dsc = {{
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = {len(chars)},
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t ui_font_zh_22 = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {ascent + descent},
    .base_line = {descent},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = &font_dsc,
}};
'''
    header = '''#ifndef UI_FONT_ZH_22_H
#define UI_FONT_ZH_22_H

#include "lvgl.h"

extern const lv_font_t ui_font_zh_22;

#endif
'''
    OUTPUT_C.write_text(source, encoding="utf-8", newline="\n")
    OUTPUT_H.write_text(header, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
