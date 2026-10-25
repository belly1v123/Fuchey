#!/usr/bin/env python3
"""Build a Fuchey character pack ("FCP1") from PNG frame folders.

A character pack is what Fuchey needs to show a marketplace character
other than the built-in Yeti on its home screen: the idle / happy / DND
clips, compressed as pixel runs, plus where the head is in every frame
so worn hats follow it. The companion app sends the file to Fuchey over
USB (char_begin / char_chunk / char_end); Fuchey stores it on LittleFS.

Frames are drawn 1:1 into the home screen's 120x120 character box, which
is the website's 96x96 base-art grid at 1.25x (the website draws a 120 px
character image into its 96 grid the same way), so worn items line up.

File "FCP1" (integers little-endian), see firmware lib/wearables/CharacterPack.hpp:
  char[4] "FCP1"
  u8      id_len, then id (a-z 0-9 -, 2..48 chars)
  i8      legs_y    grid row: in idle, items from here down don't bob
  u8      clip_count
  clips × {
    u8 mood (0 idle, 1 happy, 2 dnd), u8 fps, u8 w, u8 h, u8 frames,
    frames × { i8 dx, i8 dy, u16 run_count, runs × {u8 x, u8 y, u8 w, u16 rgb565} }
  }
dx/dy: head offset of that frame vs idle frame 0, in grid px.

Stdlib only (PNG decoding from convert_sprite.py).

Usage (from the repo root; Assets/ is local art, not in git):
  python scripts/make_character_pack.py rhino companion/packs/rhino.fcp \
      --idle  "Assets/rihno_asset/Krispa/rhino-ideal pack" \
      --happy "Assets/rihno_asset/Krispa/Rhino happy batch" \
      --dnd   "Assets/rihno_asset/Krispa/Rhino dnd proj" \
      --legs 74
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from convert_sprite import decode_png_rgba, px_at, rgb565  # noqa: E402

BOX = 120                 # home character box, screen px
GRID_SCALE = 1.25         # grid px -> box px
MOODS = {"idle": 0, "happy": 1, "dnd": 2}
FPS = {"idle": 10, "happy": 8, "dnd": 10}
SEARCH = 16               # +- box px searched around idle's head
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,47}$")


def load_frames(folder):
    """Numbered frames (`..._0001.png`) of a folder → [(w, h, pixels)], pixels[y][x] = rgb | None."""
    files = sorted(p for p in Path(folder).glob("*.png") if re.search(r"_\d{4}\.png$", p.name))
    if not files:
        sys.exit(f"error: no numbered frames (*_0001.png) in '{folder}'")
    out = []
    for p in files:
        w, h, ch, rows = decode_png_rgba(p)
        if w > BOX or h > BOX:
            sys.exit(f"error: {p.name} is {w}x{h}, bigger than the {BOX}x{BOX} box")
        px = [[None] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                r, g, b, a = px_at(rows, ch, x, y)
                if a >= 128:
                    px[y][x] = (r, g, b)
        out.append((w, h, px))
    return out


def runs_of(w, h, px):
    out = []
    for y in range(h):
        x = 0
        while x < w:
            p = px[y][x]
            if p is None:
                x += 1
                continue
            c = rgb565(*p)
            n = 1
            while x + n < w and px[y][x + n] is not None and rgb565(*px[y][x + n]) == c:
                n += 1
            out.append((x, y, n, c))
            x += n
    return out


def head_template(frame, head):
    w, h, px = frame
    if head is None:
        ys = [y for y in range(h) for x in range(w) if px[y][x] is not None]
        xs = [x for y in range(h) for x in range(w) if px[y][x] is not None]
        top, bottom = min(ys), max(ys)
        head = (min(xs), top, max(xs) + 1, top + int((bottom - top) * 0.35))
    x0, y0, x1, y1 = head
    return [(x, y, px[y][x]) for y in range(y0, y1, 2) for x in range(x0, x1, 2) if px[y][x] is not None]


def match(template, frame):
    """(dx, dy) in box px where the idle head fits this frame best (smallest shift on ties)."""
    w, h, px = frame
    best = None
    shifts = sorted(((dx, dy) for dy in range(-SEARCH, SEARCH + 1) for dx in range(-SEARCH, SEARCH + 1)),
                    key=lambda s: abs(s[0]) + abs(s[1]))
    for dx, dy in shifts:
        cost = 0
        for (x, y, c) in template:
            tx, ty = x + dx, y + dy
            q = px[ty][tx] if 0 <= tx < w and 0 <= ty < h else None
            if q is None:
                cost += 300
            else:
                cost += abs(c[0] - q[0]) + abs(c[1] - q[1]) + abs(c[2] - q[2])
            if best is not None and cost >= best[0]:
                break
        if best is None or cost < best[0]:
            best = (cost, dx, dy)
    return best[1], best[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("id")
    ap.add_argument("out")
    for m in MOODS:
        ap.add_argument(f"--{m}", help=f"folder of {m} frames" + (" (required)" if m == "idle" else ""))
    ap.add_argument("--legs", type=int, default=72, help="grid row where the legs start (default 72)")
    ap.add_argument("--head", help="head box in idle frame 0 px: x0,y0,x1,y1 (default: top 35%% of the art)")
    a = ap.parse_args()
    if not ID_RE.match(a.id):
        sys.exit("error: id must be 2..48 chars of a-z 0-9 -")
    if not a.idle:
        sys.exit("error: --idle is required")
    head = tuple(int(v) for v in a.head.split(",")) if a.head else None

    clips = {m: load_frames(getattr(a, m)) for m in MOODS if getattr(a, m)}
    template = head_template(clips["idle"][0], head)

    idb = a.id.encode()
    out = bytearray(b"FCP1")
    out += struct.pack("<B", len(idb)) + idb
    out += struct.pack("<bB", a.legs, len(clips))
    for mood, frames in clips.items():
        w, h, _ = frames[0]
        out += struct.pack("<5B", MOODS[mood], FPS[mood], w, h, len(frames))
        dxs, dys, nruns = [], [], 0
        for f in frames:
            if f[0] != w or f[1] != h:
                sys.exit(f"error: {mood} frames differ in size")
            dx, dy = match(template, f)
            gx, gy = round(dx / GRID_SCALE), round(dy / GRID_SCALE)
            runs = runs_of(*f)
            nruns += len(runs)
            dxs.append(gx)
            dys.append(gy)
            out += struct.pack("<bbH", gx, gy, len(runs))
            for (x, y, n, c) in runs:
                out += struct.pack("<BBBH", x, y, n, c)
        print(f"{mood}: {len(frames)} frames {w}x{h}, {nruns} runs, head dx={dxs} dy={dys}", file=sys.stderr)

    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(out)
    print(f"wrote {a.out} ({len(out)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
