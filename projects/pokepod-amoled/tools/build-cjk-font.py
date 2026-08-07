#!/usr/bin/env python3
"""Build native-size, 4-bit grayscale PokePod fonts from an OFL OTF/TTF."""

from __future__ import annotations

import argparse
import struct
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FIXED_TEXT = """
胶囊列表首页设置与设备详情语音微信输入录音中停止电池充电无线网络已连接未连接异常开启关闭
配网模式返回收藏归档重试播放暂无内容待转写转写中完成失败按可再次触发时间信号存储麦克风扬声器
触摸屏型号打开手机连接热点密码五分钟自动新建正在提交成功队列系统版本抬起亮屏可用不可用请插入
卡短按长按安全关机发送云端文字原文校对正文上一条下一条分钟前小时前天今天设备温度
轻触开始最长秒正在记录声音完成后会出现在还没有回到第一条刚刚需要检查选择热点名称保存成功
状态正常扫描附近网络存储字库抬起设备时屏临时等待开始结束按住松开正在输入未设置扫描附近网络
转写可阅读录音微信连接关闭打开热点名称密码配置网络腾讯转写下一步上一步保存验证清除已有密钥
一上不与中临为也云五交亮仍任会传但使保信候值停充克入关再写分列则别制功加务动卡即发取句可台同名后启和响囊回在填备失始字存安完密对将尚就屏已常幕并库应度开异式归录径待微必志态情成或手抬持按接提插播支收放整文断新无日时显暂最有未本机权条查校档检模次止正步求池法消清点热状用电留的盘码确示秒称稍空第签线络绪缺网置胶腾自藏表装触讯设证识词试话该详语请读败起超路转输过返进连送选通配重钟钥键长闭间队限除音页须频风首验麦
"""

ASCII_POINTS = list(range(0x20, 0x7F))
PUNCTUATION = "，。！？：；、“”‘’（）【】《》—…·￥"
FIXED_SIZES = (16, 20, 28, 36)
DISPLAY_TEXT = "语音胶囊微信输入正在转写胶囊设备连接手机暂无内容需要检查密码"
TIMER_TEXT = "0123456789:%-"


@lru_cache(maxsize=None)
def load_font(font_path: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(font_path, size)


def render_glyph(font_path: Path, point: int, size: int) -> tuple[int, bytes]:
    font = load_font(str(font_path), size)
    char = chr(point)
    advance = max(4, min(size, int(round(font.getlength(char)))))
    image = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(image)
    box = draw.textbbox((0, 0), char, font=font)
    width = box[2] - box[0]
    height = box[3] - box[1]
    x = (advance - width) // 2 - box[0]
    y = (size - height) // 2 - box[1]
    draw.text((x, y), char, font=font, fill=255)
    pixels = image.load()
    packed = bytearray()
    pending: int | None = None
    for row in range(size):
        for column in range(size):
            alpha = min(15, (pixels[column, row] + 8) // 17)
            if pending is None:
                pending = alpha
            else:
                packed.append((pending << 4) | alpha)
                pending = None
    if pending is not None:
        packed.append(pending << 4)
    return advance, bytes(packed)


def fixed_codepoints() -> list[int]:
    return sorted(set(ASCII_POINTS + [ord(char) for char in FIXED_TEXT if ord(char) > 127]))


def full_codepoints() -> list[int]:
    points = ASCII_POINTS + list(range(0x4E00, 0xA000))
    points.extend(ord(char) for char in PUNCTUATION)
    return sorted(set(points))


def write_binary(path: Path, font_path: Path, points: list[int], size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    bitmap_bytes = (size * size + 1) // 2
    entry_size = 8 + bitmap_bytes
    with path.open("wb") as output:
        output.write(struct.pack(
            "<4sHHBBHII", b"PKF2", size, size, 4, 0, 0, len(points), entry_size
        ))
        for point in points:
            advance, bitmap = render_glyph(font_path, point, size)
            output.write(struct.pack("<IB3x", point, advance))
            output.write(bitmap)


def points_for_size(size: int, all_points: list[int]) -> list[int]:
    if size == 28:
        return sorted(set(ASCII_POINTS + [ord(char) for char in DISPLAY_TEXT]))
    if size == 36:
        return sorted({ord(char) for char in TIMER_TEXT})
    return all_points


def write_header(path: Path, font_path: Path, points: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "namespace pokepod {",
        "",
    ]
    for size in FIXED_SIZES:
        size_points = points_for_size(size, points)
        bitmap_bytes = (size * size + 1) // 2
        type_name = f"FixedGlyph{size}"
        array_name = f"kFixedGlyphs{size}"
        lines.append(
            f"struct {type_name} {{ uint32_t codepoint; uint8_t advance; uint8_t bitmap[{bitmap_bytes}]; }};"
        )
        lines.append(f"static constexpr {type_name} {array_name}[] = {{")
        for point in size_points:
            advance, bitmap = render_glyph(font_path, point, size)
            encoded = ", ".join(f"0x{value:02x}" for value in bitmap)
            lines.append(f"  {{0x{point:04x}, {advance}, {{{encoded}}}}},")
        lines.extend([
            "};",
            f"static constexpr uint32_t {array_name}Count =",
            f"    sizeof({array_name}) / sizeof({array_name}[0]);",
            "",
        ])
    lines.extend(["}  // namespace pokepod", ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--fixed-header", type=Path, required=True)
    parser.add_argument("--sd-font", type=Path, required=True)
    args = parser.parse_args()
    if not args.font.is_file():
        raise SystemExit(f"font does not exist: {args.font}")
    points = fixed_codepoints()
    write_header(args.fixed_header, args.font, points)
    write_binary(args.sd_font, args.font, full_codepoints(), 20)


if __name__ == "__main__":
    main()
