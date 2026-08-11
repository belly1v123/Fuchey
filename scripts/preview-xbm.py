#!/usr/bin/env python3
"""Preview 1-bpp XBM-style bitmaps embedded in C++ headers.

Decodes byte arrays exactly like Display::draw_bitmap() does:
  - stride = (width + 7) / 8 bytes per row
  - MSB-first: bit (col % 8) -> pixel col, bit index 7 - (col % 8)

Verifies byte count == stride * height and renders an ASCII preview so a
mis-thresholded/inverted/truncated export is caught before flashing hardware.

Usage:
    python scripts/preview-xbm.py <header.hpp> <array_name> <width> <height>
    python scripts/preview-xbm.py firmware/lib/display/ui/Animations.hpp image_tx_fail_bits 123 30
    python scripts/preview-xbm.py firmware/lib/display/ui/Animations.hpp image_tx_success_bits 120 29
"""
import re
import sys


def find_array(source: str, name: str) -> list[int]:
    m = re.search(rf"{re.escape(name)}\s*\[\]\s*=\s*\{{(.*?)\}}", source, re.S)
    if not m:
        sys.exit(f"error: array '{name}' not found in file")
    return [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\b\d+\b", m.group(1))]


def main() -> None:
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    path, name, w_s, h_s = sys.argv[1:]
    w, h = int(w_s), int(h_s)

    with open(path, encoding="utf-8", errors="replace") as f:
        source = f.read()

    data = find_array(source, name)
    stride = (w + 7) // 8
    expected = stride * h

    print(f"{name}: {w}x{h}  stride={stride}  bytes={len(data)}  expected={expected}")
    if len(data) != expected:
        sys.exit(f"ERROR: byte count mismatch ({len(data)} != {expected})")

    print(f"\nASCII preview ('.' = off, '#' = on), every 2nd column:\n")
    for row in range(h):
        buf = []
        for col in range(0, w, 2):
            byte = data[row * stride + col // 8]
            bit = 7 - (col % 8)
            buf.append("#" if byte & (1 << bit) else ".")
        print(f"{row:2d} |{''.join(buf)}|")

    print(f"\nOK: {name} is self-consistent with draw_bitmap()" if len(data) == expected else "")


if __name__ == "__main__":
    main()