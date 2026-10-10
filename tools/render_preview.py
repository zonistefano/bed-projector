#!/usr/bin/env python3
"""Render the projector pages on the host and save them as PNG images.

Compiles LVGL (from managed_components/, present after the first idf.py
build) with main/bed_ui.c and tools/preview/preview.c using the host C
compiler, then writes one enlarged image per scenario plus an overview sheet:

    python3 tools/render_preview.py [output directory, default build/preview]
"""

from __future__ import annotations

import struct
import subprocess
import sys
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LVGL = ROOT / "managed_components/lvgl__lvgl"
SCALE = 4


def compile_all(out: Path) -> Path:
    objects = out / "obj"
    objects.mkdir(parents=True, exist_ok=True)
    flags = ["-O1", "-w", "-I", str(ROOT / "tools/preview/include"), "-I", str(ROOT / "main"),
             "-I", str(LVGL), "-I", str(LVGL / "src"),
             '-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="lv_host_config.h"']
    sources = sorted((LVGL / "src").rglob("*.c"))
    sources += [ROOT / "main" / name for name in
                ("bed_ui.c", "bed_fonts.c", "bed_icons.c", "projector_logic.c")]
    sources.append(ROOT / "tools/preview/preview.c")

    def build(source: Path) -> Path:
        target = objects / (str(source.relative_to(ROOT)).replace("/", "_") + ".o")
        if not target.exists() or target.stat().st_mtime < source.stat().st_mtime or \
                source.is_relative_to(ROOT / "main") or source.is_relative_to(ROOT / "tools"):
            subprocess.run(["cc", *flags, "-c", str(source), "-o", str(target)], check=True)
        return target

    with ThreadPoolExecutor() as pool:
        built = list(pool.map(build, sources))
    binary = out / "preview"
    subprocess.run(["cc", *map(str, built), "-o", str(binary), "-lm"], check=True)
    return binary


def png(path: Path, width: int, height: int, rows: list[bytes]) -> None:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\x00" + row for row in rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pixels(path: Path, diameter: int) -> list[list[tuple[int, int, int]]]:
    """Decode RGB565, black out what falls outside the lens circle, mark its edge."""
    data = path.read_bytes()
    image = []
    radius = diameter / 2
    for y in range(128):
        row = []
        for x in range(128):
            value = data[(y * 128 + x) * 2] | data[(y * 128 + x) * 2 + 1] << 8
            rgb = ((value >> 11) * 255 // 31, (value >> 5 & 63) * 255 // 63, (value & 31) * 255 // 31)
            distance = ((x + 0.5 - 64) ** 2 + (y + 0.5 - 64) ** 2) ** 0.5
            if distance > radius:
                rgb = (40, 44, 52) if distance < radius + 1 else (14, 15, 18)
            row.append(rgb)
        image.append(row)
    return image


def main() -> int:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/preview"
    if not LVGL.exists():
        print("managed_components/lvgl__lvgl is missing: run idf.py build once", file=sys.stderr)
        return 1
    out.mkdir(parents=True, exist_ok=True)
    binary = compile_all(out)
    for old in out.glob("*.rgb565"):
        old.unlink()
    subprocess.run([str(binary), str(out)], check=True)
    frames = sorted(out.glob("*.rgb565"), key=lambda p: p.stat().st_mtime)
    images = []
    for frame in frames:
        diameter = int(frame.stem.rsplit("_", 1)[1])
        image = pixels(frame, diameter)
        rows = [b"".join(bytes(p) * SCALE for p in row) for row in image for _ in range(SCALE)]
        png(out / f"{frame.stem}.png", 128 * SCALE, 128 * SCALE, rows)
        images.append(image)
    columns = 4
    sheet_rows = []
    for start in range(0, len(images), columns):
        group = images[start:start + columns]
        group += [[[(0, 0, 0)] * 128 for _ in range(128)]] * (columns - len(group))
        for y in range(128):
            line = b"".join(b"".join(bytes(p) * 3 for p in image[y]) + bytes(3 * 3 * 4)
                            for image in group)
            sheet_rows += [line] * 3
        sheet_rows += [bytes(len(sheet_rows[-1]))] * 12
    png(out / "overview.png", len(sheet_rows[0]) // 3, len(sheet_rows), sheet_rows)
    print(f"{len(frames)} pages in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
