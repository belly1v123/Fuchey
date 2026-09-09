#!/usr/bin/env python3
"""Crop transparent borders off Fuchey menu icon PNGs so the artwork fills
the frame, then delegate to scripts/convert_sprite.py for RGB565 headers."""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert_sprite as cs

MARGIN = 6
OUT = Path(r"C:\Users\pranj\AppData\Local\Temp\opencode\cropped")
OUT.mkdir(parents=True, exist_ok=True)

JOBS = [
    ("Assets/Wallet Info.png", 96, 96, "WalletInfoIcon"),
    ("Assets/View Balance.png", 96, 96, "ViewBalanceIcon"),
    ("Assets/Pomodoro.png", 96, 96, "PomodoroIcon"),
    ("Assets/Badge.png", 96, 96, "BadgeIcon"),
    ("Assets/SolCoin.png", 32, 32, "SolCoinIcon"),
    ("Assets/USDC.png", 32, 32, "UsdcIcon"),
    ("Assets/QR.png", 96, 96, "QrIcon"),
]


def encode_png_rgba(w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(ctype, data):
        c = struct.pack(">I", len(data)) + ctype + data
        return c + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


for src, dw, dh, name in JOBS:
    w, h, ch, rows = cs.decode_png_rgba(Path(src))
    # bbox of opaque pixels
    xs0, ys0, xs1, ys1 = w, h, -1, -1
    for y in range(h):
        for x in range(w):
            _, _, _, a = cs.px_at(rows, ch, x, y)
            if a > 127:
                if x < xs0:
                    xs0 = x
                if x > xs1:
                    xs1 = x
                if y < ys0:
                    ys0 = y
                if y > ys1:
                    ys1 = y
    if xs1 < 0:
        sys.exit(f"error: {src} fully transparent")
    xs0 = max(0, xs0 - MARGIN)
    ys0 = max(0, ys0 - MARGIN)
    xs1 = min(w - 1, xs1 + MARGIN)
    ys1 = min(h - 1, ys1 + MARGIN)
    cw, chh = xs1 - xs0 + 1, ys1 - ys0 + 1
    # make square by expanding the short side (keep glyph centered)
    side = max(cw, chh)
    pad_x = (side - cw) // 2
    pad_y = (side - chh) // 2
    xs0 = min(max(0, xs0 - pad_x), w - side)
    ys0 = min(max(0, ys0 - pad_y), h - side)
    xs0 = max(0, xs0)
    ys0 = max(0, ys0)
    side = min(side, w - xs0, h - ys0)
    cropped = []
    for y in range(ys0, ys0 + side):
        row = rows[y]
        cropped.append(row[xs0 * ch:(xs0 + side) * ch])
    # normalize to RGBA bytes for the encoder
    rgba_rows = []
    for r in cropped:
        out = bytearray()
        for i in range(0, len(r), ch):
            if ch == 4:
                out += r[i:i + 4]
            elif ch == 3:
                out += r[i:i + 3] + b"\xff"
            else:
                g = r[i]
                a = r[i + 1] if ch == 2 else 255
                out += bytes((g, g, g, a))
        rgba_rows.append(bytes(out))
    dest = OUT / f"{name}.png"
    dest.write_bytes(encode_png_rgba(side, side, rgba_rows))
    print(f"{src}: crop ({xs0},{ys0}) {side}x{side} -> {dest}")
