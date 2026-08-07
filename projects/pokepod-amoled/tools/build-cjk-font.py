#!/usr/bin/env python3
"""Build PokePod 16x16 monochrome CJK fonts from a local TTC/TTF font."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FIXED_TEXT = """
胶囊列表首页设置与设备详情语音微信输入录音中停止电池充电无线网络已连接未连接异常开启关闭
配网模式返回收藏归档重试播放暂无内容待转写转写中完成失败按可再次触发时间信号存储麦克风扬声器
触摸屏型号打开手机连接热点密码五分钟自动新建正在提交成功队列系统版本抬起亮屏可用不可用请插入
卡短按长按安全关机发送云端文字原文校对正文上一条下一条分钟前小时前天今天设备温度
随手说一句轻触开始最长秒安全保存正在记录横向滑动锁定声音已经完成后会出现在还没有回到
第一条刚刚需要检查连接手机选择热点名称密码保存成功自动关闭状态正常扫描附近网络存储字库
抬起设备时屏五分钟临时等待开始结束按住松开
一上不与中临为也云五交亮仍任会传但使保信候值停充克入关再写分列则别制功加务动卡即发取句可台同名后启和响囊回在填备失始字存安完密对将尚就屏已常幕并库应度开异式归录径待微必志态情成或手抬持按接提插播支收放整文断新无日时显暂最有未本机权条查校档检模次止正步求池法消清点热状用电留的盘码确示秒称稍空第签线络绪缺网置胶腾自藏表装触讯设证识词试话该详语请读败起超路转输过返进连送选通配重钟钥键长闭间队限除音页须频风首验麦
"""


def glyph(font: ImageFont.FreeTypeFont, char: str) -> bytes:
    image = Image.new("L", (16, 16), 0)
    draw = ImageDraw.Draw(image)
    box = font.getbbox(char)
    width = box[2] - box[0]
    height = box[3] - box[1]
    x = (16 - width) // 2 - box[0]
    y = (16 - height) // 2 - box[1]
    draw.text((x, y), char, font=font, fill=255)
    pixels = image.load()
    result = bytearray()
    for row in range(16):
        bits = 0
        for column in range(16):
            if pixels[column, row] >= 96:
                bits |= 1 << (15 - column)
        result.extend(struct.pack(">H", bits))
    return bytes(result)


def codepoints(full: bool) -> list[int]:
    if full:
        points = list(range(0x4E00, 0xA000))
        points.extend(ord(c) for c in "，。！？：；、“”‘’（）【】《》—…·￥")
        return sorted(set(points))
    return sorted({ord(c) for c in FIXED_TEXT if ord(c) > 127})


def write_binary(path: Path, font: ImageFont.FreeTypeFont, points: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(struct.pack("<4sHHII", b"PKF1", 16, 16, len(points), 36))
        for point in points:
            output.write(struct.pack("<I", point))
            output.write(glyph(font, chr(point)))


def write_header(path: Path, font: ImageFont.FreeTypeFont, points: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "namespace pokepod {",
        "",
        "struct FixedChineseGlyph { uint32_t codepoint; uint8_t bitmap[32]; };",
        "static constexpr FixedChineseGlyph kFixedChineseGlyphs[] = {",
    ]
    for point in points:
        data = glyph(font, chr(point))
        encoded = ", ".join(f"0x{value:02x}" for value in data)
        lines.append(f"  {{0x{point:04x}, {{{encoded}}}}},")
    lines.extend([
        "};",
        "static constexpr uint32_t kFixedChineseGlyphCount =",
        "    sizeof(kFixedChineseGlyphs) / sizeof(kFixedChineseGlyphs[0]);",
        "",
        "}  // namespace pokepod",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--fixed-header", type=Path, required=True)
    parser.add_argument("--sd-font", type=Path, required=True)
    args = parser.parse_args()
    font = ImageFont.truetype(str(args.font), 16)
    write_header(args.fixed_header, font, codepoints(False))
    write_binary(args.sd_font, font, codepoints(True))


if __name__ == "__main__":
    main()
