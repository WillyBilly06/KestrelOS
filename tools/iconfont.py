"""iconfont.py - turn KestrelOS's icon set into a TrueType font.

Why a font at all, when the icons already scale.

`gui_icon()` computes every coordinate from the size it is asked for, so the
icons are vector already and come out right at any resolution - they never had
the problem the text had, where a fixed-size bitmap was being doubled.  A font
fixes nothing about how they look inside this system.

What a font gives is somewhere else: a file anybody can install on Windows,
macOS or Linux and use in a document, a slide or a web page.  The icon set stops
being something only this system can draw.

So this EXPORTS rather than replaces.  The icons stay defined once, here, as
primitives in a 1000-unit square; the runtime keeps drawing them its own way and
this writes the same definitions out as glyph outlines.  One source, two
outputs, and no way for them to drift apart.

What does not survive the trip, said plainly: a glyph is one filled shape in one
colour.  Icons that shade part of themselves to suggest depth - the folder's
inner panel, the disk's face - come out flat here.  Colour fonts exist and are
supported unevenly enough that using one would defeat the point of a file that
works anywhere.

    python tools/iconfont.py [out/KestrelIcons.ttf]

The glyphs land in the Private Use Area from U+E000, which is what icon fonts
conventionally do: no real character is displaced, and anything that does not
have the font shows a placeholder rather than the wrong letter.
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The design grid.  A thousand units square, y upwards, which is what font
# outlines use - the opposite of the screen, so every y is flipped on the way
# out and the definitions below can be read the way the icon looks.
EM = 1000
ASCENT = 800
DESCENT = -200


# --------------------------------------------------------------------- shapes
#
# Every primitive returns a list of contours, and a contour is a list of
# (x, y) points.  Straight lines only: these are geometric shapes and a
# quadratic curve would buy nothing but a harder file to check.
#
# Winding matters and is the one thing that is silently wrong if got backwards.
# TrueType fills by non-zero winding, so an outer boundary and the hole inside
# it must go opposite ways round or the hole fills in.  Outer contours here run
# clockwise in font space; holes run anticlockwise.

def _area(points):
    """Twice the signed area.  Positive means anticlockwise."""
    total = 0
    for i in range(len(points)):
        x1, y1 = points[i]
        x2, y2 = points[(i + 1) % len(points)]
        total += x1 * y2 - x2 * y1
    return total


def _wind(points, clockwise):
    """Put a contour the way round it needs to go.

    Ordering points by hand and trusting the result is how a hole ends up
    filled in: the two contours of a frame were both written top-left first and
    both came out clockwise, so the non-zero fill counted two windings and the
    middle stayed solid.  Nothing about the file is invalid when that happens -
    it renders, as a filled box - so it has to be enforced rather than
    inspected."""
    want_negative = clockwise
    if (_area(points) < 0) != want_negative:
        points = list(reversed(points))
    return points


def rect(x, y, w, h):
    """A filled rectangle."""
    return [_wind([(x, y + h), (x + w, y + h), (x + w, y), (x, y)], True)]


def frame(x, y, w, h, t):
    """A rectangle outline: the outside, and the inside as a hole.

    The hole runs the opposite way round from the outside, which is what makes
    it a hole rather than a second solid rectangle on top of the first."""
    outer = _wind([(x, y + h), (x + w, y + h), (x + w, y), (x, y)], True)
    inner = _wind([(x + t, y + t), (x + t, y + h - t),
                   (x + w - t, y + h - t), (x + w - t, y + t)], False)
    return [outer, inner]


def hline(x, y, w, t):
    return rect(x, y - t // 2, w, t)


def vline(x, y, h, t):
    return rect(x - t // 2, y, t, h)


def line(x1, y1, x2, y2, t):
    """A stroke, as the quadrilateral it covers.

    A font has no notion of a line with a width - only filled shapes - so the
    stroke is turned into the four corners of the band it sweeps.  Caps are
    square, which is what the runtime draws too."""
    dx, dy = x2 - x1, y2 - y1
    length = (dx * dx + dy * dy) ** 0.5
    if length < 1e-6:
        return rect(x1 - t // 2, y1 - t // 2, t, t)

    # The perpendicular, half a stroke wide.
    px = -dy / length * (t / 2.0)
    py = dx / length * (t / 2.0)

    return [[(round(x1 + px), round(y1 + py)),
             (round(x2 + px), round(y2 + py)),
             (round(x2 - px), round(y2 - py)),
             (round(x1 - px), round(y1 - py))]]


def polygon(points):
    return [_wind(list(points), True)]


def ring(cx, cy, r, t, steps=32):
    """A circle outline, as two rings of straight segments."""
    import math
    outer, inner = [], []
    for i in range(steps):
        a = 2.0 * math.pi * i / steps
        outer.append((round(cx + math.cos(a) * r), round(cy + math.sin(a) * r)))
        inner.append((round(cx + math.cos(a) * (r - t)),
                      round(cy + math.sin(a) * (r - t))))
    return [_wind(outer, True), _wind(inner, False)]


def disc(cx, cy, r, steps=32):
    import math
    pts = []
    for i in range(steps):
        a = 2.0 * math.pi * i / steps
        pts.append((round(cx + math.cos(a) * r), round(cy + math.sin(a) * r)))
    return [_wind(pts, True)]


def shapes(*groups):
    out = []
    for g in groups:
        out.extend(g)
    return out


# ---------------------------------------------------------------- the icons
#
# The same shapes gui_icon() draws, written once at a thousand units so both
# the runtime and this file can be read against each other.  Names match the
# icon_id enum in user/libgui/gui.h.
#
# Proportions are taken from the drawing code: it works in quarters of the
# icon's size, so a quarter here is 250.

Q = EM // 4          # the unit the drawing code thinks in
T = 70               # a stroke, about what a 1px line is at 16px

ICONS = {
    "terminal": lambda: shapes(
        frame(60, 100, 880, 800, T),
        line(230, 660, 400, 500, T),
        line(400, 500, 230, 340, T),
        hline(520, 320, 300, T)),

    "folder": lambda: shapes(
        rect(60, 140, 880, 560),
        rect(60, 700, 420, 120)),

    "file": lambda: shapes(
        frame(160, 100, 680, 800, T),
        hline(300, 640, 400, T),
        hline(300, 500, 400, T),
        hline(300, 360, 260, T)),

    "disk": lambda: shapes(
        frame(80, 200, 840, 600, T),
        disc(500, 500, 130)),

    "log": lambda: shapes(
        frame(120, 120, 760, 760, T),
        hline(260, 680, 480, T),
        hline(260, 540, 480, T),
        hline(260, 400, 480, T),
        hline(260, 260, 300, T)),

    "info": lambda: shapes(
        ring(500, 500, 420, T),
        rect(455, 300, 90, 320),
        rect(455, 660, 90, 90)),

    "settings": lambda: shapes(
        ring(500, 500, 250, T),
        vline(500, 760, 180, T),
        vline(500, 60, 180, T),
        hline(60, 500, 180, T),
        hline(760, 500, 180, T)),

    "power": lambda: shapes(
        ring(500, 460, 330, T),
        rect(455, 560, 90, 380)),

    "close": lambda: shapes(
        line(180, 180, 820, 820, T),
        line(180, 820, 820, 180, T)),

    "minimise": lambda: hline(180, 260, 640, T),

    "maximise": lambda: frame(180, 180, 640, 640, T),

    "restore": lambda: shapes(
        frame(120, 120, 560, 560, T),
        frame(320, 320, 560, 560, T)),

    "arrow_up": lambda: polygon([(500, 860), (860, 400), (140, 400)]),
    "arrow_down": lambda: polygon([(500, 140), (140, 600), (860, 600)]),
    "arrow_left": lambda: polygon([(140, 500), (600, 860), (600, 140)]),
    "arrow_right": lambda: polygon([(860, 500), (400, 140), (400, 860)]),

    "check": lambda: shapes(
        line(150, 500, 400, 250, T + 20),
        line(400, 250, 860, 720, T + 20)),

    "warning": lambda: shapes(
        polygon([(500, 900), (940, 120), (60, 120)]),
        rect(455, 300, 90, 320),
        rect(455, 660, 90, 90)),

    "kestrel": lambda: shapes(
        ring(500, 500, 430, T),
        line(200, 300, 800, 700, T),
        line(200, 700, 800, 300, T),
        vline(500, 70, 860, T)),

    "display": lambda: shapes(
        frame(80, 300, 840, 600, T),
        hline(320, 160, 360, T + 30)),

    "usb": lambda: shapes(
        vline(500, 180, 660, T),
        disc(500, 140, 90),
        line(500, 560, 720, 700, T),
        line(500, 420, 300, 560, T)),

    "wifi": lambda: shapes(
        disc(500, 180, 90),
        ring(500, 180, 330, T),
        ring(500, 180, 560, T)),

    "sound": lambda: shapes(
        polygon([(180, 380), (380, 380), (620, 160), (620, 840), (380, 620),
                 (180, 620)]),
        ring(660, 500, 220, T)),

    "palette": lambda: shapes(
        ring(500, 500, 430, T),
        disc(380, 620, 80),
        disc(620, 620, 80),
        disc(500, 340, 80)),

    "chip": lambda: shapes(
        frame(220, 220, 560, 560, T),
        vline(360, 780, 160, T), vline(500, 780, 160, T), vline(640, 780, 160, T),
        vline(360, 60, 160, T), vline(500, 60, 160, T), vline(640, 60, 160, T),
        hline(60, 360, 160, T), hline(60, 500, 160, T), hline(60, 640, 160, T),
        hline(780, 360, 160, T), hline(780, 500, 160, T), hline(780, 640, 160, T)),

    "network": lambda: shapes(
        frame(360, 660, 280, 240, T),
        frame(60, 100, 280, 240, T),
        frame(660, 100, 280, 240, T),
        vline(500, 460, 200, T),
        hline(200, 460, 600, T),
        vline(200, 340, 120, T),
        vline(800, 340, 120, T)),

    "search": lambda: shapes(
        ring(420, 580, 300, T),
        line(620, 380, 880, 120, T + 20)),

    "browser": lambda: shapes(
        ring(500, 500, 430, T),
        vline(500, 70, 860, T),
        hline(70, 500, 860, T)),

    "store": lambda: shapes(
        frame(120, 120, 760, 560, T),
        polygon([(120, 680), (880, 680), (760, 900), (240, 900)])),

    "lock": lambda: shapes(
        frame(180, 100, 640, 480, T),
        ring(500, 620, 220, T)),

    "reload": lambda: shapes(
        ring(500, 500, 340, T),
        polygon([(760, 620), (940, 620), (850, 820)])),

    "home": lambda: shapes(
        polygon([(500, 920), (60, 520), (940, 520)]),
        frame(200, 100, 600, 440, T)),

    "install": lambda: shapes(
        vline(500, 340, 500, T + 20),
        polygon([(500, 220), (300, 460), (700, 460)]),
        hline(160, 120, 680, T)),

    "editor": lambda: shapes(
        line(180, 300, 700, 820, T + 40),
        polygon([(120, 120), (300, 200), (200, 300)])),
}


# ------------------------------------------------------------ the font file
#
# A TrueType file is a directory of tables.  Nine of them are the minimum a
# system will accept, and each is written below in the order the specification
# describes it, because a table that is merely plausible is a font that installs
# and then renders nothing.

def _pad(b):
    return b + b"\0" * ((4 - len(b) % 4) % 4)


def _checksum(b):
    b = _pad(b)
    total = 0
    for i in range(0, len(b), 4):
        total = (total + struct.unpack(">I", b[i:i + 4])[0]) & 0xFFFFFFFF
    return total


def glyph_data(contours):
    """One glyph in the format the `glyf` table wants.

    Points are all on-curve - these are straight-sided shapes - and each is
    stored as a delta from the one before, in as few bytes as it fits."""
    if not contours:
        return b""

    xs, ys, ends = [], [], []
    n = 0
    for c in contours:
        for (x, y) in c:
            xs.append(int(round(x)))
            ys.append(int(round(y)))
        n += len(c)
        ends.append(n - 1)

    out = struct.pack(">hhhhh", len(contours),
                      min(xs), min(ys), max(xs), max(ys))
    out += b"".join(struct.pack(">H", e) for e in ends)
    out += struct.pack(">H", 0)          # no hinting instructions

    # Flags: bit 0 says the point is on the curve.  Repeats are not compressed;
    # the file is small and a wrong repeat count is a corrupt glyph.
    out += bytes([0x01]) * len(xs)

    for values in (xs, ys):
        prev = 0
        for v in values:
            d = v - prev
            prev = v
            out += struct.pack(">h", d)

    return _pad(out)


def build(path):
    names = list(ICONS.keys())

    # Glyph 0 is .notdef and must exist; a font without it is rejected outright.
    glyphs = [b""]
    advances = [EM]
    boxes = [(0, 0, 0, 0)]

    for name in names:
        contours = ICONS[name]()
        glyphs.append(glyph_data(contours))
        advances.append(EM)
        xs = [p[0] for c in contours for p in c]
        ys = [p[1] for c in contours for p in c]
        boxes.append((min(xs), min(ys), max(xs), max(ys)))

    # ---- glyf and loca ----
    glyf = b"".join(glyphs)
    offsets, at = [], 0
    for g in glyphs:
        offsets.append(at)
        at += len(g)
    offsets.append(at)
    # The long form: offsets in bytes rather than halves, so a glyph may sit at
    # an odd multiple of two without the table having to lie about it.
    loca = b"".join(struct.pack(">I", o) for o in offsets)

    all_x0 = min(b[0] for b in boxes[1:])
    all_y0 = min(b[1] for b in boxes[1:])
    all_x1 = max(b[2] for b in boxes[1:])
    all_y1 = max(b[3] for b in boxes[1:])

    head = struct.pack(">IIIIHHqqhhhhHHhhh",
                       0x00010000, 0x00010000, 0, 0x5F0F3CF5,
                       0x000B, EM, 0, 0,
                       all_x0, all_y0, all_x1, all_y1,
                       0, 8, 2, 0, 1)
    # indexToLocFormat = 1 (long) is the second-to-last field; rewrite it.
    head = head[:50] + struct.pack(">hh", 1, 0)

    hhea = struct.pack(">IhhhHhhhhhhhHHHHh",
                       0x00010000, ASCENT, DESCENT, 0,
                       EM, 0, 0, EM,
                       1, 0, 0, 0, 0, 0, 0, 0,
                       len(glyphs))

    # The real maximums, counted.
    #
    # These were written as 64 points and 32 contours because those looked
    # generous.  Twelve of the thirty-four glyphs have more points than that -
    # anything with a circle in it - and a rasteriser that believes the table
    # simply does not draw them.  The font loads, the glyphs are in the file,
    # and half the set is invisible with nothing reporting why.
    max_points = 0
    max_contours = 0
    for name in names:
        cs = ICONS[name]()
        max_points = max(max_points, sum(len(c) for c in cs))
        max_contours = max(max_contours, len(cs))

    maxp = struct.pack(">IHHHHHHHHHHHHHH",
                       0x00010000, len(glyphs),
                       max_points, max_contours,
                       0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0)

    hmtx = b"".join(struct.pack(">Hh", advances[i], boxes[i][0])
                    for i in range(len(glyphs)))

    # ---- cmap: format 4, one segment per glyph in the private use area ----
    codes = [0xE000 + i for i in range(len(names))]
    segs = [(c, c, gid + 1) for gid, c in enumerate(codes)]
    segs.append((0xFFFF, 0xFFFF, 0))
    n = len(segs)

    import math
    search = 2 ** int(math.floor(math.log2(n))) * 2
    sub = struct.pack(">HHHHHHH", 4, 16 + 8 * n, 0, n * 2,
                      search, int(math.log2(search // 2)), n * 2 - search)
    sub += b"".join(struct.pack(">H", s[1]) for s in segs)
    sub += struct.pack(">H", 0)
    sub += b"".join(struct.pack(">H", s[0]) for s in segs)
    # idDelta maps a code to its glyph directly; idRangeOffset stays zero.
    sub += b"".join(struct.pack(">h", (s[2] - s[0]) & 0xFFFF if s[2] else 0)
                    for s in segs)
    sub += b"".join(struct.pack(">H", 0) for _ in segs)

    cmap = struct.pack(">HHHHI", 0, 1, 3, 1, 12) + sub

    # ---- name: what the font is called wherever it is installed ----
    strings = [(1, "KestrelOS Icons"), (2, "Regular"),
               (3, "KestrelOS Icons Regular"), (4, "KestrelOS Icons"),
               (5, "Version 1.000"), (6, "KestrelOSIcons-Regular")]
    records, blob = b"", b""
    for nid, text in strings:
        data = text.encode("utf-16-be")
        records += struct.pack(">HHHHHH", 3, 1, 0x409, nid, len(data), len(blob))
        blob += data
    name = struct.pack(">HHH", 0, len(strings), 6 + 12 * len(strings)) + records + blob

    os2 = struct.pack(">HhHHHhhhhhhhhhhhh10sIIII4sHHHhhh",
                      4, EM // 2, 400, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                      b"\0" * 10, 0, 0, 0, 0, b"KSTL",
                      0, min(codes), max(codes),
                      ASCENT, -DESCENT, 0)
    os2 += struct.pack(">HHhhh", 0, 0, ASCENT, DESCENT, 0)

    post = struct.pack(">IIhhIIIII", 0x00030000, 0, 0, 0, 0, 0, 0, 0, 0)

    tables = {b"OS/2": os2, b"cmap": cmap, b"glyf": glyf, b"head": head,
              b"hhea": hhea, b"hmtx": hmtx, b"loca": loca, b"maxp": maxp,
              b"name": name, b"post": post}

    keys = sorted(tables)
    count = len(keys)
    search = 2 ** int(math.floor(math.log2(count))) * 16
    header = struct.pack(">IHHHH", 0x00010000, count, search,
                         int(math.log2(search // 16)), count * 16 - search)

    offset = len(header) + 16 * count
    directory, body = b"", b""
    for k in keys:
        data = _pad(tables[k])
        directory += struct.pack(">4sIII", k, _checksum(data), offset, len(tables[k]))
        body += data
        offset += len(data)

    font = header + directory + body

    with open(path, "wb") as fh:
        fh.write(font)

    return names, codes, len(font)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "out", "KestrelIcons.ttf")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    names, codes, size = build(out)
    print("-> %s (%d glyphs, %d bytes)"
          % (os.path.relpath(out, ROOT), len(names), size))
    print()
    for name, code in zip(names, codes):
        print("    U+%04X  %s" % (code, name))


if __name__ == "__main__":
    main()
