#!/usr/bin/env python3
"""Render the KestrelOS logo - a detailed kestrel (falcon) in a stoop/glide.

Outputs to <out_dir>:
  logo_mask.png   - white silhouette on transparent, the tintable mark used in
                    the taskbar / start menu / headers (drawn in the theme accent)
  logo_tile.png   - the falcon in white on a rounded gradient tile, the full
                    'About' / brand app icon

Supersampled 4x for clean edges.  The bird: a small head with a hooked beak, a
body, two long pointed swept-back wings with primary-feather notches, and a
notched tail - a kestrel silhouette, not a plain chevron.
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

SS = 4
N  = 256
S  = N * SS

def falcon(draw, S, col):
    """A sleek peregrine/kestrel in a glide, seen head-on: long pointed wings
    swept back into the classic falcon 'anchor', a slim body, hooked beak and a
    tapered notched tail."""
    cx = S / 2
    # --- body: slim, tapered ---
    draw.polygon([
        (cx,           S*0.235),
        (cx+S*0.038,   S*0.34),
        (cx+S*0.030,   S*0.58),
        (cx,           S*0.66),
        (cx-S*0.030,   S*0.58),
        (cx-S*0.038,   S*0.34),
    ], fill=col)
    # --- head + hooked beak ---
    r = S*0.055
    hy = S*0.185
    draw.ellipse([cx-r, hy-r, cx+r, hy+r], fill=col)
    draw.polygon([(cx-r*0.4, hy-S*0.008), (cx-r*1.9, hy+S*0.02),
                  (cx-r*0.9, hy+S*0.028), (cx-r*0.3, hy+S*0.02)], fill=col)  # hooked beak
    # --- wings: swept back to a sharp point, with 3 primary feathers ---
    def wing(sign):
        sh   = (cx + sign*S*0.028, S*0.315)      # shoulder at the body
        bend = (cx + sign*S*0.175, S*0.255)      # wrist / leading bend, raised
        pts = [
            sh, bend,
            (cx + sign*S*0.475, S*0.40),         # primary 1 - the swept tip
            (cx + sign*S*0.395, S*0.405),
            (cx + sign*S*0.455, S*0.455),        # primary 2
            (cx + sign*S*0.375, S*0.455),
            (cx + sign*S*0.415, S*0.505),        # primary 3
            (cx + sign*S*0.135, S*0.52),         # trailing edge to lower body
            (cx + sign*S*0.030, S*0.44),
        ]
        draw.polygon(pts, fill=col)
    wing(-1); wing(+1)
    # --- tail: long, tapered, with a central notch ---
    draw.polygon([
        (cx-S*0.030, S*0.58),
        (cx-S*0.058, S*0.87),
        (cx-S*0.012, S*0.815),
        (cx,         S*0.85),
        (cx+S*0.012, S*0.815),
        (cx+S*0.058, S*0.87),
        (cx+S*0.030, S*0.58),
    ], fill=col)

def render_mask():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    falcon(d, S, (255, 255, 255, 255))
    # punch an eye hole
    er = S*0.016
    ex, ey = S/2 + S*0.017, S*0.175
    d.ellipse([ex-er, ey-er, ex+er, ey+er], fill=(0, 0, 0, 0))
    return img.resize((N, N), Image.LANCZOS)

def render_tile():
    top, bot = (0x6E, 0x92, 0xF7), (0x2E, 0x4E, 0xC8)
    R = int(S*0.235)
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0)); px = grad.load()
    for y in range(S):
        t = y/(S-1); c = tuple(int(top[i]+(bot[i]-top[i])*t) for i in range(3))
        for x in range(S): px[x, y] = (c[0], c[1], c[2], 255)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S-1, S-1], radius=R, fill=255)
    tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    tile.paste(grad, (0, 0), mask)
    bird = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    bd = ImageDraw.Draw(bird)
    falcon(bd, S, (255, 255, 255, 255))
    er = S*0.016; ex, ey = S/2 + S*0.017, S*0.175
    # eye as a tinted dot rather than a hole, so it reads on white
    bd.ellipse([ex-er, ey-er, ex+er, ey+er], fill=(0x2E, 0x4E, 0xC8, 255))
    tile = Image.alpha_composite(tile, bird)
    return tile.resize((N, N), Image.LANCZOS)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out/icons"
    os.makedirs(out, exist_ok=True)
    render_mask().save(os.path.join(out, "logo_mask.png"))
    render_tile().save(os.path.join(out, "logo_tile.png"))
    print("wrote logo_mask.png + logo_tile.png to", out)

if __name__ == "__main__":
    main()
