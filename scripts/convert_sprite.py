#!/usr/bin/env python3
"""Convert PNG frame(s) to RGB565 C arrays for Display::draw_sprite().

Same no-dependency style as scripts/png-to-xbm.py (stdlib only).
Source pixels are converted R8G8B8 -> RGB565 (CPU order, no swap;
the ST7789 driver byte-swaps once on blit -- see St7789::put_px).

Alpha: pixels with alpha <= 127 are mapped to the transparent color-key
(default magenta 0xF81F) so draw_sprite_transparent() can skip them.
Opaque pixels keep their RGB565 value.

Modes:
  1) Folder of frames:  python scripts/convert_sprite.py frames_dir AnimName out.hpp
  2) Horizontal strip:   python scripts/convert_sprite.py strip.png AnimName out.hpp --strip --frames 12
  3) Downscale:          ... --width 96 --height 96  (nearest-neighbor)

Guards:
  - Fails if total bytes (w*h*2*frames) > 512KB unless --force.
    Larger animations belong in SPIFFS as .bin (use --bin for raw output).

Limits reminder (ST7789 @8MHz, 240x240):
  - 96x96/frame  = 18KB SPI via push_window -> 10-12fps comfortable.
  - 240x240/frame = 115KB SPI via push_frame -> ~8fps max.
  - UI tick is 100ms (10Hz); fps > 12 is wasted on this bus.

Output header defines:
    static const uint16_t <Name>_data[] = {...};  // frames back-to-back
    static const Fuchey::SpriteAnim <Name> = {...}; // needs SpritePlayer.hpp
"""
import struct
import sys
import zlib
from pathlib import Path

MAX_BYTES = 512 * 1024
TRANSPARENT_DEFAULT = 0xF81F


def decode_png_rgba(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"error: '{path}' is not a PNG")
    pos, w = 8, None
    width = height = None
    bit_depth = color_type = None
    interlace = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if color_type == 6:
        channels = 4
    elif color_type == 2:
        channels = 3
    elif color_type in (0, 3):
        channels = 2
    else:
        sys.exit(f"error: unsupported PNG color type {color_type} in '{path}'")
    if interlace != 0:
        sys.exit("error: interlaced PNG not supported")
    if bit_depth != 8:
        sys.exit(f"error: only 8-bit PNGs supported (got {bit_depth})")
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        filt = raw[off]
        line = bytearray(raw[off + 1:off + 1 + stride])
        off += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if filt == 1:
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, channels, rows


def px_at(rows, channels, x, y):
    row = rows[y]
    o = x * channels
    if channels == 4:
        return row[o], row[o + 1], row[o + 2], row[o + 3]
    if channels == 3:
        return row[o], row[o + 1], row[o + 2], 255
    # gray/palette-promoted: luma + alpha-ish
    g = row[o]
    a = row[o + 1] if channels == 2 else 255
    return g, g, g, a


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def frame_to_rgb565(rows, channels, sw, sh, dw, dh, transparent):
    out = []
    for y in range(dh):
        sy = y * sh // dh
        for x in range(dw):
            sx = x * sw // dw
            r, g, b, a = px_at(rows, channels, sx, sy)
            if a <= 127:
                out.append(transparent)
            else:
                out.append(rgb565(r, g, b))
    return out


def main() -> None:
    args = sys.argv[1:]
    if len(args) < 3:
        sys.exit(__doc__)
    src, name, out = args[0], args[1], args[2]
    frames_n = 0
    is_strip = "--strip" in args
    force = "--force" in args
    as_bin = "--bin" in args
    transparent = TRANSPARENT_DEFAULT
    dw = dh = 0
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames_n = int(args[i + 1])
        if a == "--width" and i + 1 < len(args):
            dw = int(args[i + 1])
        if a == "--height" and i + 1 < len(args):
            dh = int(args[i + 1])
        if a == "--transparent" and i + 1 < len(args):
            transparent = int(args[i + 1], 0)

    srcp = Path(src)
    frame_rgbs = []  # list of (sw, sh, rows, channels)
    if srcp.is_dir():
        pngs = sorted(srcp.glob("*.png"))
        if not pngs:
            sys.exit(f"error: no PNGs in '{src}'")
        for f in pngs:
            w, h, ch, rows = decode_png_rgba(f)
            frame_rgbs.append((w, h, ch, rows))
    else:
        w, h, ch, rows = decode_png_rgba(srcp)
        if is_strip:
            if frames_n <= 0:
                sys.exit("error: --strip requires --frames N")
            fw = w // frames_n
            for i in range(frames_n):
                # slice rows horizontally
                sliced = []
                for row in rows:
                    sliced.append(row[i * fw * ch:(i + 1) * fw * ch])
                frame_rgbs.append((fw, h, ch, sliced))
        else:
            frame_rgbs.append((w, h, ch, rows))

    n = len(frame_rgbs)
    sw0 = frame_rgbs[0][0]
    sh0 = frame_rgbs[0][1]
    ow, oh = (dw or sw0), (dh or sh0)
    all_px = []
    for (sw, sh, ch, rows) in frame_rgbs:
        all_px.extend(frame_to_rgb565(rows, ch, sw, sh, ow, oh, transparent))

    total = len(all_px) * 2
    print(f"{name}: {ow}x{oh} x{n} frames = {total} bytes", file=sys.stderr)
    if total > MAX_BYTES and not force:
        sys.exit(f"error: {total} bytes > {MAX_BYTES} guard. Downscale or pass --force.")

    outp = Path(out)
    if as_bin:
        outp.write_bytes(struct.pack(f"<{len(all_px)}H", *all_px))
        print(f"wrote {outp} ({total} bytes raw RGB565 LE)", file=sys.stderr)
        return

    lines = []
    lines.append("#pragma once")
    lines.append(f"// Auto-generated by scripts/convert_sprite.py from '{src}'.")
    lines.append(f"// {ow}x{oh} x{n} frames, {total} bytes RGB565. Do not hand-edit.")
    lines.append("#include <cstdint>")
    lines.append('#include "SpritePlayer.hpp"')
    lines.append("")
    lines.append(f"static const uint16_t {name}_data[] = {{")
    for i in range(0, len(all_px), 12):
        chunk = ", ".join(f"0x{v:04x}" for v in all_px[i:i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const Fuchey::SpriteAnim {name} = {{")
    lines.append(f"    {name}_data, {ow}, {oh}, {n}, 10, true,")
    lines.append("};")
    outp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {outp}", file=sys.stderr)


if __name__ == "__main__":
    main()
