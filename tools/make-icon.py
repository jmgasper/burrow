#!/usr/bin/env python3
"""Build Burrow's native Haiku icon: a secure tunnel with a golden key.

The 64-unit drawing uses Haiku's 2:1 front / 1:1 receding perspective,
upper-left lighting, a two-unit silhouette outline and a rightward shadow.
All geometry is authored here; no raster tracing or external artwork is needed.

    python3 tools/make-icon.py resources/branding/burrow-icon.hvif [preview.png]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif  # noqa: E402

C = hvif.hex_color
icon = {'styles': [], 'paths': [], 'shapes': []}


def solid(color, alpha=255):
    icon['styles'].append({'color': C(color, alpha)})
    return len(icon['styles']) - 1


def gradient(start, end, colors):
    icon['styles'].append(hvif.linear_gradient(start, end,
        [(i / (len(colors) - 1), C(c)) for i, c in enumerate(colors)]))
    return len(icon['styles']) - 1


def curve(point, incoming, outgoing):
    return point, incoming, outgoing


def draw(style, points, closed=True, stroke=0, **extra):
    index = len(icon['paths'])
    icon['paths'].append({'closed': closed, 'points': points})
    shape = {'style': style, 'paths': [index], **extra}
    if stroke:
        shape['transformers'] = [{'type': 'stroke', 'width': stroke,
                                  'join': 1, 'cap': 1}]
    icon['shapes'].append(shape)
    return index


def fill(style, index):
    icon['shapes'].append({'style': style, 'paths': [index]})


# The tunnel mouth lies in the plane y = v + x / 2. Its extrusion
# recedes along (14, -14); upright edges stay vertical.
black = solid('171b20')
shadow = solid('000000', 55)
roof = gradient((12, 13), (52, 35), ['d5e8ee', '6f9fb6', '37576e'])
front = gradient((10, 24), (36, 57), ['d9edf2', '83b4ce', '4783a7'])
inside = gradient((20, 31), (33, 47), ['172b3d', '31566c'])
edge = solid('eff8fa')
shade = solid('2c536d')
gold = gradient((17, 36), (30, 51), ['fff1a5', 'efbb43', 'b57920'])
gold_edge = solid('fff4bd')
key_shadow = solid('102332', 115)

# A crisp, translucent ground shadow extending horizontally to the right.
draw(shadow, [(8, 44), (39, 60), (62, 48), (54, 43), (28, 34)])

# Outer silhouette, including the curved roof and the shaded right wall.
body = [
    (8, 44), (8, 30),
    curve((23, 19), (15, 15), (23, 19)),
    (37, 5),
    curve((54, 30), (48, 10), (54, 30)),
    (54, 44), (40, 58),
]
p = draw(black, body, stroke=2)
fill(roof, p)

# Tunnel mouth: a broad stone arch, with an unbroken outer outline.
face = [
    (8, 44), (8, 30),
    curve((24, 19), (8, 15), (40, 27)),
    (40, 44), (40, 58),
    (32, 54), (32, 40),
    curve((16, 32), (32, 29), (16, 32)),
    (16, 48),
]
p = draw(black, face, stroke=2)
fill(front, p)

# Opening and inner wall. The dark aperture remains legible at 16 pixels.
opening = [
    (16, 48), (16, 33),
    curve((24, 29), (16, 25), (32, 33)),
    (32, 40), (32, 54),
]
draw(inside, opening)
draw(shade, [(16, 48), (16, 34), (21, 29), (21, 43), (32, 49), (32, 54)])

# A narrow highlight follows the light-facing lip; no decorative texture.
draw(edge, [(10, 42), (10, 30),
    curve((24, 21), (10, 18), (37, 27)), (38, 42)],
    closed=False, stroke=1)
draw(shade, [(40, 44), (40, 56), (52, 44)], closed=False, stroke=1)

# Golden key laid across the threshold. A simple ring and two teeth
# communicate authentication without an extra shield or tiny decoration.
key = [
    curve((14, 35), (10, 35), (18, 35)),
    curve((21, 41), (21, 37), (21, 42)),
    (33, 47), (33, 51), (29, 49), (29, 53), (25, 51), (25, 47),
    (19, 44),
    curve((14, 47), (18, 47), (10, 47)),
    curve((7, 41), (7, 45), (7, 37)),
]
draw(key_shadow, [
    ((x + 2, y + 2), (xi + 2, yi + 2), (xo + 2, yo + 2))
    for (x, y), (xi, yi), (xo, yo) in map(hvif.normalize_point, key)])
p = draw(black, key, stroke=2)
fill(gold, p)
# The ring's hole is intentionally large enough to survive the mini icon.
hole = [
    curve((14, 39), (12, 39), (16, 39)),
    curve((17, 41), (17, 40), (17, 43)),
    curve((14, 43), (16, 43), (12, 43)),
    curve((11, 41), (11, 42), (11, 39)),
]
draw(black, hole)
draw(gold_edge, [(20, 42), (31, 47.5)], closed=False, stroke=1)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'burrow-icon.hvif'
    data = hvif.encode(icon)
    decoded = hvif.decode(data)
    assert hvif.encode(decoded) == data
    with open(out, 'wb') as f:
        f.write(data)
    print('%s: %d bytes' % (out, len(data)))
    svg = out.rsplit('.', 1)[0] + '.svg'
    with open(svg, 'w') as f:
        f.write(hvif.to_svg(decoded))
    print('%s: SVG source' % svg)
    if len(sys.argv) > 2:
        hvif.preview(decoded, 256).save(sys.argv[2])
        print(sys.argv[2])
