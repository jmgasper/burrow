#!/usr/bin/env python3
"""Build Burrow's application icon: a grassy hill with a tunnel entrance (a
light at its end) and a blue shield with a keyhole, readable from 16 px up.

    python3 tools/make-icon.py resources/branding/burrow-icon.hvif [preview.png]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif  # noqa: E402

C = hvif.hex_color
K = 0.5523


def curve(point, pin, pout):
    return (point, pin, pout)


def circle(cx, cy, r):
    k = K * r
    return {'closed': True, 'points': [
        curve((cx + r, cy), (cx + r, cy - k), (cx + r, cy + k)),
        curve((cx, cy + r), (cx + k, cy + r), (cx - k, cy + r)),
        curve((cx - r, cy), (cx - r, cy + k), (cx - r, cy - k)),
        curve((cx, cy - r), (cx - k, cy - r), (cx + k, cy - r)),
    ]}


def arch(left, right, base, spring):
    """A round-topped opening from base up to spring height, then a semicircle."""
    r = (right - left) / 2.0
    cx = left + r
    k = K * r
    return {'closed': True, 'points': [
        (left, base),
        curve((left, spring), (left, spring), (left, spring - k)),
        curve((cx, spring - r), (cx - k, spring - r), (cx + k, spring - r)),
        curve((right, spring), (right, spring - k), (right, spring)),
        (right, base),
    ]}


# ------------------------------------------------------------------ styles
OUTLINE = {'color': C('3b2716')}
HILL = hvif.linear_gradient((0, 12), (0, 55), [
    (0.0, C('9ad35a')), (0.30, C('5ea83a')), (0.335, C('4a8a2c')),
    (0.36, C('a0703f')), (0.75, C('7b5130')), (1.0, C('5c3a20'))])
TUNNEL = hvif.radial_gradient((32, 50), 11, [
    (0.0, C('2a1a0e')), (0.6, C('1a1009')), (1.0, C('0c0805'))])
GLOW = hvif.radial_gradient((32, 49), 4.5, [
    (0.0, C('fff4b8')), (0.45, C('ffd35a', 200)), (1.0, C('ffb020', 0))])
SHIELD = hvif.linear_gradient((44, 34), (62, 60), [(0.0, C('5aa9ff')), (1.0, C('1d5fc4'))])
SHIELD_OUTLINE = {'color': C('0f2f66')}
KEYHOLE = {'color': C('ffffff')}
SHADOW = hvif.radial_gradient((32, 58), 24, [
    (0.0, (20, 20, 20, 110)), (0.7, (20, 20, 20, 40)), (1.0, (20, 20, 20, 0))], ratio=0.16)
HIGHLIGHT = {'color': (255, 255, 255, 70)}

style_names = ['outline', 'hill', 'tunnel', 'glow', 'shield', 'shieldOutline', 'keyhole', 'shadow',
               'highlight']
styles = [OUTLINE, HILL, TUNNEL, GLOW, SHIELD, SHIELD_OUTLINE, KEYHOLE, SHADOW, HIGHLIGHT]
S = {name: index for index, name in enumerate(style_names)}

# ------------------------------------------------------------------ paths
hill = {'closed': True, 'points': [
    curve((3, 55), (3, 55), (6, 38)),
    curve((32, 12), (13, 12), (51, 12)),
    curve((61, 55), (58, 38), (61, 55)),
]}
tunnel = arch(22, 42, 55, 45)
glow = circle(32, 49, 4.5)
shield = {'closed': True, 'points': [
    curve((53, 34), (49, 36.5), (57, 36.5)),
    (62, 37),
    curve((62, 46), (62, 46), (62, 53)),
    curve((53, 61), (58, 59), (48, 59)),
    curve((44, 46), (44, 53), (44, 46)),
    (44, 37),
]}
keyhole = {'closed': True, 'points': [
    curve((53, 42.5), (51.6, 42.5), (54.4, 42.5)),
    curve((55.5, 45), (55.5, 43.6), (55.5, 46)),
    (54.3, 46.6),
    (55, 52.5),
    (51, 52.5),
    (51.7, 46.6),
    curve((50.5, 45), (50.5, 46), (50.5, 43.6)),
]}
shadow = circle(32, 58, 1)
highlight = {'closed': False, 'points': [curve((14, 22), (14, 22), (20, 15.5)), curve((32, 13.5), (26, 13.5), (32, 13.5))]}

path_names = ['hill', 'tunnel', 'glow', 'shield', 'keyhole', 'shadow', 'highlight']
paths = [hill, tunnel, glow, shield, keyhole, shadow, highlight]
P = {name: index for index, name in enumerate(path_names)}


def shape(style, *names, **extra):
    result = {'style': S[style], 'paths': [P[n] for n in names]}
    result.update(extra)
    return result


thin = {'type': 'stroke', 'width': 1.5, 'join': 2, 'cap': 1, 'miter': 4}
contour_large = {'type': 'contour', 'width': 3, 'join': 2, 'miter': 4}
contour_small = {'type': 'contour', 'width': 2, 'join': 2, 'miter': 4}

SMALL = {'lod': (0.0, 0.5)}
LARGE = {'lod': (0.5, 4.0)}
DETAIL = {'lod': (0.95, 4.0)}

shapes = [
    shape('shadow', 'shadow', matrix=[26.0, 0.0, 0.0, 4.0, 32.0 - 32.0 * 26.0, 58.0 - 58.0 * 4.0]),
    shape('outline', 'hill', transformers=[contour_large], **LARGE),
    shape('outline', 'hill', transformers=[contour_small], **SMALL),
    shape('hill', 'hill'),
    shape('highlight', 'highlight', transformers=[thin], **DETAIL),
    shape('outline', 'tunnel', transformers=[contour_small], **LARGE),
    shape('tunnel', 'tunnel'),
    shape('glow', 'glow', **LARGE),
    shape('shieldOutline', 'shield', transformers=[contour_large], **LARGE),
    shape('shieldOutline', 'shield', transformers=[contour_small], **SMALL),
    shape('shield', 'shield'),
    shape('keyhole', 'keyhole', **LARGE),
]

icon = {'styles': styles, 'paths': paths, 'shapes': shapes}

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'burrow-icon.hvif'
    data = hvif.encode(icon)
    hvif.decode(data)
    with open(out, 'wb') as f:
        f.write(data)
    print('%s: %d bytes' % (out, len(data)))
    svg = out.rsplit('.', 1)[0] + '.svg'
    with open(svg, 'w') as f:
        f.write(hvif.to_svg(icon))
    print('%s: SVG source' % svg)
    if len(sys.argv) > 2:
        hvif.preview(icon, 256, (245, 245, 240, 255)).save(sys.argv[2])
        print(sys.argv[2])
