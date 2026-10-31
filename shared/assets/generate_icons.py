"""Renders every ClipDeck icon from one design.

Stacked cards with a </> mark on a transparent background; a transparent gap
separates each card from the one behind it so the layers stay distinct on light
and dark surfaces alike. Writes:

- windows/icon-*.png and windows/clipdeck.ico (Windows app, taskbar and tray)
- Assets.xcassets/AppIcon.appiconset/icon-*.png (macOS app icon; Linux
  packaging installs the 64, 128 and 256 px files as the launcher icon)
- clipdeck.svg (macOS menu bar and Linux tray, shown at 18-24 px)

    python -I generate_icons.py      # needs Pillow; run from anywhere
"""

import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
WINDOWS_DIR = os.path.join(HERE, "windows")
APPICON_DIR = os.path.join(HERE, "Assets.xcassets", "AppIcon.appiconset")
CANVAS = 1024
PNG_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
APPICON_SIZES = [16, 32, 64, 128, 256, 512, 1024]

# Back to front: top colour, bottom colour, rotation (degrees), offset.
FULL_STACK = [
    ((0x4A, 0x6C, 0xC4), (0x2B, 0x46, 0x92), 15, (-150, -18)),
    ((0x3A, 0x86, 0xF0), (0x20, 0x5C, 0xCC), 7, (-74, -6)),
    ((0x6C, 0xC4, 0xFF), (0x1E, 0x78, 0xF2), 0, (0, 0)),
]
# Below 32 px three cards turn to mush; one card behind the front reads better.
SMALL_STACK = [
    ((0x3A, 0x70, 0xDC), (0x22, 0x4C, 0xB0), 12, (-150, -14)),
    ((0x6C, 0xC4, 0xFF), (0x1E, 0x78, 0xF2), 0, (0, 0)),
]
FULL = dict(gap=26, stroke=50)
SMALL = dict(gap=44, stroke=86)
GLYPH = (0x08, 0x22, 0x52, 255)
CARD = (250, 250, 560, 640)  # front card left, top, width, height on the canvas
RADIUS = 112
# The </> mark, relative to the front card's centre at a 560 px card width.
GLYPH_STROKES = [
    [(-70, -80), (-160, 0), (-70, 80)],
    [(70, -80), (160, 0), (70, 80)],
    [(34, -118), (-34, 118)],
]


def card_mask(rotation, offset, grow=0):
    left, top, width, height = CARD
    mask = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (left - grow, top - grow, left + width + grow, top + height + grow),
        radius=RADIUS + grow, fill=255)
    center = (left + width / 2, top + height / 2)
    mask = mask.rotate(rotation, resample=Image.BICUBIC, center=center)
    return ImageChops.offset(mask, offset[0], offset[1])


def gradient(top, bottom):
    column = Image.new("RGB", (1, CANVAS))
    for y in range(CANVAS):
        t = y / (CANVAS - 1)
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return column.resize((CANVAS, CANVAS))


def glyph_points():
    left, top, width, height = CARD
    cx, cy = left + width / 2, top + height / 2
    s = width / 560
    return [[(cx + x * s, cy + y * s) for x, y in points] for points in GLYPH_STROKES]


def draw_glyph(image, stroke):
    """The </> mark on the front card, with round caps and joins."""
    draw = ImageDraw.Draw(image)
    r = stroke / 2
    for points in glyph_points():
        draw.line(points, fill=GLYPH, width=int(stroke), joint="curve")
        for x, y in points:
            draw.ellipse((x - r, y - r, x + r, y + r), fill=GLYPH)


def render_canvas(stack, gap, stroke):
    image = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    for top, bottom, rotation, offset in stack:
        # Knock a gap out of everything already drawn behind this card.
        knockout = card_mask(rotation, offset, grow=gap)
        alpha = ImageChops.subtract(image.getchannel("A"), knockout)
        image.putalpha(alpha)

        mask = card_mask(rotation, offset)
        layer = gradient(top, bottom).convert("RGBA")
        layer.putalpha(mask)
        # A soft light edge along the top gives each card some thickness.
        edge = ImageChops.subtract(mask, ImageChops.offset(mask, 0, 10))
        edge = edge.filter(ImageFilter.GaussianBlur(3)).point(lambda v: v * 0.35)
        layer = Image.composite(Image.new("RGBA", layer.size, (255, 255, 255, 255)), layer, edge)
        layer.putalpha(mask)
        image = Image.alpha_composite(image, layer)
    draw_glyph(image, stroke)
    return image


def square_box(image):
    """The artwork's bounding square with a small margin: (left, top, side)."""
    left, top, right, bottom = image.getchannel("A").getbbox()
    width, height = right - left, bottom - top
    side = int(max(width, height) * 1.06)
    return (left - (side - width) // 2, top - (side - height) // 2, side)


def render(stack, gap, stroke):
    """The artwork centred in a square with a small margin."""
    image = render_canvas(stack, gap, stroke)
    left, top, side = square_box(image)
    return image.crop((left, top, left + side, top + side))


def card_rect(grow=0):
    left, top, width, height = CARD
    return (f'x="{left - grow}" y="{top - grow}" width="{width + 2 * grow}" '
            f'height="{height + 2 * grow}" rx="{RADIUS + grow}"')


def card_transform(rotation, offset):
    left, top, width, height = CARD
    cx, cy = left + width / 2, top + height / 2
    # PIL rotates counter-clockwise; SVG's rotate() turns clockwise in y-down space.
    return f'translate({offset[0]} {offset[1]}) rotate({-rotation} {cx:g} {cy:g})'


def svg(stack, gap, stroke):
    """The same stack as vector art; each card masks out a gap behind it."""
    left, top, side = square_box(render_canvas(stack, gap, stroke))
    hexed = lambda colour: "#%02x%02x%02x" % colour[:3]
    defs, body = [], []
    for index, (top_colour, bottom_colour, rotation, offset) in enumerate(stack):
        defs.append(
            f'<linearGradient id="card{index}" gradientUnits="userSpaceOnUse" '
            f'x1="0" y1="0" x2="0" y2="{CANVAS}">'
            f'<stop offset="0" stop-color="{hexed(top_colour)}"/>'
            f'<stop offset="1" stop-color="{hexed(bottom_colour)}"/></linearGradient>')
        card = (f'<rect {card_rect()} transform="{card_transform(rotation, offset)}" '
                f'fill="url(#card{index})"/>')
        cutouts = "".join(
            f'<rect {card_rect(gap)} transform="{card_transform(r, o)}" fill="#000"/>'
            for _, _, r, o in stack[index + 1:])
        if cutouts:
            defs.append(
                f'<mask id="gap{index}" maskUnits="userSpaceOnUse" x="0" y="0" '
                f'width="{CANVAS}" height="{CANVAS}"><rect width="{CANVAS}" height="{CANVAS}" '
                f'fill="#fff"/>{cutouts}</mask>')
            body.append(f'<g mask="url(#gap{index})">{card}</g>')
        else:
            body.append(card)
    path = " ".join("M" + " L".join(f"{x:g} {y:g}" for x, y in points) for points in glyph_points())
    body.append(f'<path d="{path}" fill="none" stroke="{hexed(GLYPH)}" stroke-width="{stroke}" '
                'stroke-linecap="round" stroke-linejoin="round"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" '
            f'viewBox="{left} {top} {side} {side}">\n'
            '  <defs>\n    ' + "\n    ".join(defs) + '\n  </defs>\n  '
            + "\n  ".join(body) + "\n</svg>\n")


def main():
    full = render(FULL_STACK, **FULL)
    small = render(SMALL_STACK, **SMALL)
    images = {}
    for size in PNG_SIZES:
        source = small if size < 32 else full
        images[size] = source.resize((size, size), Image.LANCZOS)
        images[size].save(os.path.join(WINDOWS_DIR, f"icon-{size}.png"))
    images[256].save(os.path.join(WINDOWS_DIR, "clipdeck.ico"), sizes=[(s, s) for s in ICO_SIZES],
                     append_images=[images[s] for s in ICO_SIZES if s != 256])
    for size in APPICON_SIZES:
        source = small if size < 32 else full
        source.resize((size, size), Image.LANCZOS).save(os.path.join(APPICON_DIR, f"icon-{size}.png"))
    # The menu bar / tray icon is only ever small: the two-card stack reads best.
    with open(os.path.join(HERE, "clipdeck.svg"), "w", encoding="utf-8", newline="\n") as file:
        file.write(svg(SMALL_STACK, **SMALL))
    print("wrote windows/, Assets.xcassets/AppIcon.appiconset/ and clipdeck.svg")


if __name__ == "__main__":
    sys.exit(main())
