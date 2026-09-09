#!/usr/bin/env python3
"""Convert a PNG icon to a 1-bpp XBM-style C array for Display::draw_bitmap().

The Display driver (lib/display/Display.cpp) draws bitmaps in the same format
u8g2 calls "XBM": row-major, byte-padded rows, MSB-first (bit 7 = left-most
pixel of the byte). See scripts/preview-xbm.py to render/verify the result.

Conversion rules:
  - Source is expected to be a flat icon with transparent background.
  - A pixel is ON if its alpha channel > 127 (fully opaque white icons convert
    cleanly; semi-transparent anti-aliased edges are dropped).
  - If target w/h are given and smaller than the source, nearest-neighbor
    downsampling is applied (keeps hard edges crisp for 1-bpp output).

Usage:
    python scripts/png-to-xbm.py <input.png> <array_name> [out_width out_height]
Output:
    Prints the C declaration on stdout (copy into a header).
Examples:
    python scripts/png-to-xbm.py firmware/assets/weather_cloud.bmp image_weather_cloud_bits 17 16
    python scripts/png-to-xbm.py firmware/assets/temperature.bmp image_temperature_bits 16 16
"""
import struct
import sys
import zlib


def decode_png_rgba(path: str) -> tuple[int, int, list[bytes]]:
    """Decode an (ideally non-interlaced) PNG into RGBA rows. Returns (w, h, rows)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"error: '{path}' is not a PNG")

    pos = 8
    width = height = None
    bit_depth = color_type = None
    interlace = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        ctype = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break

    if color_type == 6:  # RGBA
        channels = 4
    elif color_type == 2:  # RGB
        channels = 3
    elif color_type == 0 or color_type == 3:  # gray / palette -> promote via gray+alpha
        channels = 2
    else:
        sys.exit(f"error: unsupported PNG color type {color_type}")

    if interlace != 0:
        sys.exit("error: interlaced PNG not supported")
    if bit_depth != 8:
        sys.exit(f"error: only 8-bit PNGs supported (got depth {bit_depth})")
    if width is None or height is None:
        sys.exit(f"error: IHDR missing in '{path}'")

    raw = zlib.decompress(idat)
    stride = width * channels
    rows: list[bytes] = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        if off >= len(raw):
            break
        filt = raw[off]
        line = bytearray(raw[off + 1 : off + 1 + stride])
        off += 1 + stride
        for x in range(stride):
            a = line[x - channels] if x >= channels else 0
            b = prev[x]
            c = prev[x - channels] if x >= channels else 0
            if filt == 1:  # Sub
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:  # Up
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:  # Average
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:  # Paeth
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        rows.append(bytes(line))
        prev = line
    return width, height, rows


def to_1bit(rows: list[bytes], channels: int, w: int, h: int,
            out_w: int = 0, out_h: int = 0) -> list[int]:
    """Threshold RGBA rows to a 1-bpp XBM-padded byte list.

    If source is larger than target, block max-pooling is applied: a target
    pixel is ON if ANY source pixel in its block is opaque. This keeps thin
    1-bpp icon strokes visible at small sizes (nearest-neighbor alone would
    drop sparse white-on-transparent shapes).
    """
    src_w = len(rows[0]) // channels
    src_h = len(rows)
    if out_w and out_h and (out_w, out_h) != (src_w, src_h):
        dw, dh = out_w, out_h
    else:
        dw, dh = src_w, src_h

    stride = (dw + 7) // 8
    bits: list[int] = [0] * (stride * dh)
    for y in range(dh):
        y0 = y * src_h // dh
        y1 = max(y0 + 1, (y + 1) * src_h // dh)
        for x in range(dw):
            x0 = x * src_w // dw
            x1 = max(x0 + 1, (x + 1) * src_w // dw)
            on = False
            for sy in range(y0, y1):
                row = rows[sy]
                for sx in range(x0, x1):
                    o = sx * channels
                    alpha = row[o + 3] if channels == 4 else (
                        row[o + 1] if channels == 2 else 255)
                    if alpha > 127:
                        on = True
                        break
                if on:
                    break
            if on:
                bits[y * stride + x // 8] |= 0x80 >> (x % 8)
    return bits


def main() -> None:
    if len(sys.argv) not in (3, 5):
        sys.exit(__doc__)
    path, name = sys.argv[1], sys.argv[2]
    out_w = int(sys.argv[3]) if len(sys.argv) == 5 else 0
    out_h = int(sys.argv[4]) if len(sys.argv) == 5 else 0

    w, h, rows = decode_png_rgba(path)
    channels = len(rows[0]) // max(w, 1)
    data = to_1bit(rows, channels, w, h, out_w, out_h)

    dw = out_w if out_w else w
    dh = out_h if out_h else h
    stride = (dw + 7) // 8
    expected = stride * dh
    if len(data) != expected:
        sys.exit(f"error: internal mismatch {len(data)} != {expected}")

    print(f"// {name}: {dw}x{dh}, stride={stride}, source=({w}x{h}).")
    print(f"static const uint8_t {name}[] = {{")
    cells = [f"0x{v:02x}" for v in data]
    lines = []
    cur = "    "
    for i, cell in enumerate(cells):
        piece = ("" if i == 0 else ", ") + cell
        if len(cur) + len(piece) > 96:
            lines.append(cur + ",")
            cur = "    " + cell
        else:
            cur += piece
    lines.append(cur)
    print("\n".join(lines))
    print("};")


if __name__ == "__main__":
    main()