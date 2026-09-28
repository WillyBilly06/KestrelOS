#!/usr/bin/env python3
"""Generate KestrelOS app icons as real RGBA images.

A modern flat set: each icon is a rounded-square tile with a soft vertical
gradient and a clean white glyph, supersampled 4x for smooth edges and written
as an RGBA PNG with transparent corners.  These replace the single-colour
line-drawings gui_icon() used to render, which read as amateur next to the rest
of the interface.

  python tools/genicons.py <out_dir>

Writes <out_dir>/<name>.png for each app, 128x128 RGBA.
"""
import os, sys, math
from PIL import Image, ImageDraw, ImageFilter

SS = 4                      # supersample factor
N  = 128                    # final size
S  = N * SS                 # working size
R  = int(S * 0.235)         # corner radius (macOS-ish squircle-ish)

def lerp(a, b, t): return tuple(int(a[i] + (b[i]-a[i])*t) for i in range(3))

def tile(top, bot):
    """A rounded-square tile with a vertical gradient top->bot, plus a soft
    inner top highlight for a little depth."""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    px = grad.load()
    for y in range(S):
        c = lerp(top, bot, y / (S - 1))
        for x in range(S):
            px[x, y] = (c[0], c[1], c[2], 255)
    # rounded-rect mask
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S-1, S-1], radius=R, fill=255)
    img.paste(grad, (0, 0), mask)
    # soft top sheen
    sheen = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sheen)
    sd.rounded_rectangle([0, 0, S-1, int(S*0.5)], radius=R,
                         fill=(255, 255, 255, 34))
    sheen = sheen.filter(ImageFilter.GaussianBlur(S*0.04))
    img = Image.alpha_composite(img, Image.composite(
        sheen, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))
    return img, mask

def draw(img, fn):
    d = ImageDraw.Draw(img)
    fn(d, S)
    return img

W = (255, 255, 255, 255)     # glyph white
def wa(a): return (255, 255, 255, a)

# ---- per-app glyphs (drawn in a 0..S space, centred) ----------------------

def g_browser(d, S):
    cx = cy = S//2; r = int(S*0.30); lw = max(2, S//42)
    d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=W, width=lw)
    d.line([cx-r, cy, cx+r, cy], fill=W, width=lw)
    d.arc([cx-int(r*0.55), cy-r, cx+int(r*0.55), cy+r], 0, 360, fill=W, width=lw)
    d.arc([cx-r, cy-int(r*0.55), cx+r, cy+int(r*0.55)], 0, 360, fill=W, width=lw)

def g_store(d, S):
    r = int(S*0.085); g = int(S*0.11); cx = cy = S//2
    for iy in range(2):
        for ix in range(2):
            x = cx + (ix*2-1)*g - (g if ix==0 else 0) + (0)
            x0 = cx - g*2 + ix*(g*2+ r*2)
            y0 = cy - g*2 + iy*(g*2 + r*2)
            d.rounded_rectangle([x0, y0, x0+r*2, y0+r*2], radius=r*0.5, fill=W)

def g_terminal(d, S):
    m = int(S*0.30); lw = max(2, S//34)
    d.line([m, m, m+int(S*0.16), S//2], fill=W, width=lw, joint="curve")
    d.line([m+int(S*0.16), S//2, m, S-m], fill=W, width=lw, joint="curve")
    d.line([S//2+int(S*0.02), S-m, S-m, S-m], fill=W, width=lw)

def g_files(d, S):
    x0, y0, x1, y1 = int(S*0.24), int(S*0.32), int(S*0.76), int(S*0.70)
    d.rounded_rectangle([x0, int(S*0.27), x0+int(S*0.22), int(S*0.40)], radius=S*0.03, fill=W)
    d.rounded_rectangle([x0, y0, x1, y1], radius=S*0.045, fill=W)

def g_events(d, S):
    x0, x1 = int(S*0.28), int(S*0.72); lw = max(2, S//30)
    for i, y in enumerate([0.34, 0.46, 0.58]):
        yy = int(S*y)
        d.line([x0, yy, x1 - (int(S*0.14) if i==2 else 0), yy], fill=W, width=lw)
    # warning dot
    d.ellipse([int(S*0.60), int(S*0.55), int(S*0.60)+int(S*0.11), int(S*0.55)+int(S*0.11)], fill=W)

def g_system(d, S):
    cx = cy = S//2; r = int(S*0.28); lw = max(2, S//30)
    d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=W, width=lw)
    d.ellipse([cx-int(S*0.03), cy-int(S*0.17), cx+int(S*0.03), cy-int(S*0.11)], fill=W)
    d.rounded_rectangle([cx-int(S*0.028), cy-int(S*0.06), cx+int(S*0.028), cy+int(S*0.17)], radius=S*0.02, fill=W)

def g_task(d, S):
    base = int(S*0.70); x = int(S*0.30); bw = int(S*0.10); gap = int(S*0.055)
    for h in [0.16, 0.28, 0.22, 0.34]:
        d.rounded_rectangle([x, base-int(S*h), x+bw, base], radius=S*0.02, fill=W)
        x += bw+gap

def g_3d(d, S):
    cx = cy = S//2; r = int(S*0.26); lw = max(2, S//34)
    top=(cx, cy-r); rgt=(cx+int(r*0.87), cy-int(r*0.5)); btr=(cx+int(r*0.87), cy+int(r*0.5))
    bot=(cx, cy+r); btl=(cx-int(r*0.87), cy+int(r*0.5)); tpl=(cx-int(r*0.87), cy-int(r*0.5))
    d.line([top,rgt,bot,btl,top], fill=W, width=lw, joint="curve")
    d.line([tpl,top], fill=W, width=lw); d.line([btr,bot], fill=W, width=lw)
    d.line([tpl,btl], fill=W, width=lw); d.line([tpl,(cx,cy)], fill=W, width=lw)
    d.line([rgt,(cx,cy)], fill=W, width=lw); d.line([(cx,cy),bot], fill=W, width=lw)

def g_editor(d, S):
    x0,y0,x1,y1 = int(S*0.30), int(S*0.26), int(S*0.70), int(S*0.74); lw=max(2,S//40)
    d.rounded_rectangle([x0,y0,x1,y1], radius=S*0.04, outline=W, width=lw)
    for y in [0.38,0.48,0.58]:
        d.line([x0+int(S*0.07), int(S*y), x1-int(S*0.07), int(S*y)], fill=W, width=lw)

def g_install(d, S):
    cx=S//2; lw=max(2,S//28)
    d.line([cx, int(S*0.26), cx, int(S*0.58)], fill=W, width=lw)
    d.line([cx-int(S*0.12), int(S*0.44), cx, int(S*0.60)], fill=W, width=lw, joint="curve")
    d.line([cx+int(S*0.12), int(S*0.44), cx, int(S*0.60)], fill=W, width=lw, joint="curve")
    d.line([int(S*0.30), int(S*0.70), int(S*0.70), int(S*0.70)], fill=W, width=lw)

def g_settings(d, S):
    cx=cy=S//2; R1=int(S*0.30); R2=int(S*0.135); teeth=8; lw=max(2,S//30)
    for i in range(teeth):
        a=i*math.pi*2/teeth
        x0=cx+int(math.cos(a)*R1*0.8); y0=cy+int(math.sin(a)*R1*0.8)
        x1=cx+int(math.cos(a)*R1);     y1=cy+int(math.sin(a)*R1)
        d.line([x0,y0,x1,y1], fill=W, width=int(S*0.05))
    d.ellipse([cx-R1+int(S*0.03),cy-R1+int(S*0.03),cx+R1-int(S*0.03),cy+R1-int(S*0.03)], outline=W, width=int(S*0.05))
    d.ellipse([cx-R2,cy-R2,cx+R2,cy+R2], fill=None, outline=W, width=lw)

def g_devices(d, S):
    x0,y0,x1,y1=int(S*0.30),int(S*0.30),int(S*0.70),int(S*0.70); lw=max(2,S//34)
    d.rounded_rectangle([x0,y0,x1,y1], radius=S*0.03, outline=W, width=lw)
    d.rectangle([int(S*0.42),int(S*0.42),int(S*0.58),int(S*0.58)], fill=W)
    for t in [0.40,0.5,0.60]:
        d.line([int(S*t),y0,int(S*t),y0-int(S*0.06)], fill=W, width=lw)
        d.line([int(S*t),y1,int(S*t),y1+int(S*0.06)], fill=W, width=lw)
        d.line([x0,int(S*t),x0-int(S*0.06),int(S*t)], fill=W, width=lw)
        d.line([x1,int(S*t),x1+int(S*0.06),int(S*t)], fill=W, width=lw)

def g_about(d, S):
    # a stylised kestrel chevron
    cx=S//2
    d.line([int(S*0.28),int(S*0.44),cx,int(S*0.30),int(S*0.72),int(S*0.44)], fill=W, width=int(S*0.055), joint="curve")
    d.line([int(S*0.36),int(S*0.58),cx,int(S*0.46),int(S*0.64),int(S*0.58)], fill=W, width=int(S*0.05), joint="curve")

# name -> (top colour, bottom colour, glyph).  Colours are plain (R,G,B).
ICONS = {
    "browser":  ((0x3E,0x9A,0xF6), (0x1C,0x63,0xC8), g_browser),
    "store":    ((0x9B,0x6C,0xF8), (0x6D,0x28,0xD9), g_store),
    "terminal": ((0x3A,0x42,0x4E), (0x20,0x25,0x2E), g_terminal),
    "files":    ((0xFF,0xCB,0x5A), (0xEA,0x9A,0x2A), g_files),
    "events":   ((0xFB,0x8C,0x3A), (0xEA,0x6A,0x1E), g_events),
    "system":   ((0x3D,0xA0,0xF2), (0x21,0x66,0xCE), g_system),
    "task":     ((0x3B,0xD9,0x8F), (0x18,0x9E,0x63), g_task),
    "gl":       ((0x2A,0xCF,0xCF), (0x0E,0x9B,0x9B), g_3d),
    "editor":   ((0x9A,0xAA,0xC0), (0x64,0x74,0x8B), g_editor),
    "install":  ((0x3B,0xD8,0x84), (0x1F,0xA6,0x5C), g_install),
    "settings": ((0x93,0x9E,0xAD), (0x5B,0x64,0x72), g_settings),
    "devices":  ((0x53,0x8B,0xF0), (0x2E,0x62,0xC0), g_devices),
    "about":    ((0x6E,0x92,0xF7), (0x33,0x54,0xD9), g_about),
    "kestrel":  ((0x6E,0x92,0xF7), (0x33,0x54,0xD9), g_about),
}

def parse(c):
    return c  # already (R,G,B)

# ---- monochrome control glyphs -------------------------------------------
#
# These are NOT tiles: they are white shapes on a transparent field, meant to
# be drawn tinted (gui_app_icon_tinted) so the power button and the window
# frame buttons are real images that still follow the theme and light up on
# hover.  Drawn thick so they stay crisp when scaled down to ~18 px.

def gy_power(d, S):
    cx = cy = S // 2
    r = int(S * 0.30); lw = int(S * 0.085)
    # ring with a gap at the top, plus the stem through it
    d.arc([cx - r, cy - r, cx + r, cy + r], 300, 240, fill=W, width=lw)
    d.line([cx, int(S * 0.18), cx, cy], fill=W, width=lw)

def gy_close(d, S):
    m = int(S * 0.30); lw = int(S * 0.09)
    d.line([m, m, S - m, S - m], fill=W, width=lw)
    d.line([S - m, m, m, S - m], fill=W, width=lw)

def gy_min(d, S):
    m = int(S * 0.30); lw = int(S * 0.09)
    d.line([m, int(S * 0.60), S - m, int(S * 0.60)], fill=W, width=lw)

def gy_max(d, S):
    m = int(S * 0.30); lw = int(S * 0.075)
    d.rounded_rectangle([m, m, S - m, S - m], radius=int(S * 0.05),
                        outline=W, width=lw)

def gy_restore(d, S):
    lw = int(S * 0.07); a = int(S * 0.26); b = int(S * 0.62)
    off = int(S * 0.10)
    d.rounded_rectangle([a + off, a, b + off, b], radius=int(S * 0.045),
                        outline=W, width=lw)              # back square
    d.rounded_rectangle([a, a + off, b, b + off], radius=int(S * 0.045),
                        outline=W, width=lw)              # front square

GLYPHS = {
    "glyph_power":   gy_power,
    "glyph_close":   gy_close,
    "glyph_min":     gy_min,
    "glyph_max":     gy_max,
    "glyph_restore": gy_restore,
}

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "out/icons"
    os.makedirs(out, exist_ok=True)
    for name, (top, bot, glyph) in ICONS.items():
        img, mask = tile(top, bot)
        glyphimg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        draw(glyphimg, glyph)
        # keep the glyph inside the tile
        glyphimg = Image.composite(glyphimg, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask)
        img = Image.alpha_composite(img, glyphimg)
        img = img.resize((N, N), Image.LANCZOS)
        img.save(os.path.join(out, name + ".png"))
    for name, glyph in GLYPHS.items():
        g = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        draw(g, glyph)
        g = g.resize((N, N), Image.LANCZOS)
        g.save(os.path.join(out, name + ".png"))
    print("wrote %d icons + %d glyphs to %s" % (len(ICONS), len(GLYPHS), out))

if __name__ == "__main__":
    main()
