#!/usr/bin/env python3
"""Build native-size, 4-bit grayscale PokePod fonts from an OFL OTF/TTF."""

from __future__ import annotations

import argparse
import struct
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ASCII_POINTS = list(range(0x20, 0x7F))
PUNCTUATION = "，。！？：；、“”‘’（）【】《》—…·￥"
FIXED_SIZES = (16, 20, 28, 36)
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


def default_ui_sources() -> list[Path]:
    source_dir = Path(__file__).parents[1] / "firmware" / "PokePodAmoled"
    return sorted(
        path for path in source_dir.iterdir()
        if path.suffix in {".cpp", ".h", ".ino"}
        and path.name != "FixedChineseFont.h"
    )


def source_ui_characters(paths: list[Path]) -> set[str]:
    characters: set[str] = set(PUNCTUATION)
    for path in paths:
        source = path.read_text(encoding="utf-8")
        characters.update(
            char for char in source
            if 0x3400 <= ord(char) <= 0x4DBF
            or 0x4E00 <= ord(char) <= 0x9FFF
            or 0xF900 <= ord(char) <= 0xFAFF
            or char in PUNCTUATION
        )
    return characters


def fixed_codepoints(paths: list[Path]) -> list[int]:
    return sorted(set(ASCII_POINTS + [ord(char) for char in source_ui_characters(paths)]))


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
        "// Generated from every Chinese character and UI punctuation mark in PokePodAmoled source.",
        "// Regenerate with tools/build-cjk-font.py; do not hand-edit glyphs.",
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
    parser.add_argument("--sd-font", type=Path)
    parser.add_argument("--source", type=Path, action="append")
    args = parser.parse_args()
    if not args.font.is_file():
        raise SystemExit(f"font does not exist: {args.font}")
    sources = args.source or default_ui_sources()
    missing_sources = [source for source in sources if not source.is_file()]
    if missing_sources:
        raise SystemExit(f"source does not exist: {missing_sources[0]}")
    points = fixed_codepoints(sources)
    write_header(args.fixed_header, args.font, points)
    if args.sd_font is not None:
        write_binary(args.sd_font, args.font, full_codepoints(), 20)


if __name__ == "__main__":
    main()
