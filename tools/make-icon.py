#!/usr/bin/env python3
"""Build Burrow's application icon from its logo (resources/branding/burrow-logo.png):
a burrow in an earth mound, with grass and rocks, and a silver and blue shield
with a keyhole in the tunnel.

The mound, grass and rocks are the traced layers in burrow-icon-layers.json
(see tools/trace-icon.py); the shield is drawn here, from the artwork's
measured outline, split into a lit left half and a shaded right half.

    python3 tools/make-icon.py resources/branding/burrow-icon.hvif [preview.png]
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif  # noqa: E402

C = hvif.hex_color
K = 0.5523
LAYERS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      'resources/branding/burrow-icon-layers.json')

icon = {'styles': [], 'paths': [], 'shapes': []}


def style(definition):
    icon['styles'].append(definition)
    return len(icon['styles']) - 1


def path(points):
    icon['paths'].append({'closed': True, 'points': points})
    return len(icon['paths']) - 1


def shape(style_index, paths, **extra):
    result = {'style': style_index, 'paths': paths}
    result.update(extra)
    icon['shapes'].append(result)


def node(values):
    """A traced point: [x, y] for a corner, or [x, y, in_x, in_y, out_x, out_y]."""
    if len(values) == 2:
        return tuple(values)
    return (tuple(values[0:2]), tuple(values[2:4]), tuple(values[4:6]))


def curve(point, pin, pout):
    return (point, pin, pout)


def contour(width):
    return [{'type': 'contour', 'width': width, 'join': 2, 'miter': 4}]


# ------------------------------------------------------------------ traced artwork
with open(LAYERS) as f:
    for layer in json.load(f)['layers']:
        shape(style({'color': C(layer['color'])}), [path([node(p) for p in points]) for points in layer['paths']])

# ------------------------------------------------------------------ shield
# Seen slightly from the left: the halves meet on x = 36.76.
CX, TIP = 36.76, 36.72
rim = [
    curve((CX, 29.62), (33.26, 31.9), (40.06, 31.8)),
    curve((44.75, 32.94), (41.25, 32.24), (44.75, 32.94)),
    curve((44.75, 37.0), (44.75, 37.0), (44.75, 44.0)),
    curve((TIP, 50.25), (40.66, 48.14), (31.14, 47.99)),
    curve((27.47, 37.0), (27.47, 44.5), (27.47, 37.0)),
    curve((27.47, 33.0), (27.47, 33.0), (31.47, 32.2)),
]
rim_left = [
    curve((CX, 29.62), (33.26, 31.9), (CX, 29.62)),
    curve((TIP, 50.25), (TIP, 50.25), (31.14, 47.99)),
    rim[4],
    rim[5],
]
face = [
    curve((CX, 31.72), (34.3, 33.27), (39.3, 33.27)),
    curve((43.53, 34.2), (41.03, 33.65), (43.53, 34.2)),
    curve((43.53, 36.5), (43.53, 36.5), (43.53, 42.5)),
    curve((TIP, 48.31), (40.2, 46.29), (31.67, 46.21)),
    curve((29.21, 36.5), (29.21, 43.0), (29.21, 36.5)),
    curve((29.21, 34.55), (29.21, 34.55), (32.01, 33.85)),
]
face_left = [
    curve((CX, 31.72), (34.3, 33.27), (CX, 31.72)),
    curve((TIP, 48.31), (TIP, 48.31), (31.67, 46.21)),
    face[4],
    face[5],
]
# A round keyhole on a tapered stem.
kx, ky, r = 36.82, 38.55, 1.78
k = K * r
keyhole = [
    (kx - 0.86, 40.4),
    curve((kx - r, ky), (kx - r, ky + 0.7 * k), (kx - r, ky - k)),
    curve((kx, ky - r), (kx - k, ky - r), (kx + k, ky - r)),
    curve((kx + r, ky), (kx + r, ky - k), (kx + r, ky + 0.7 * k)),
    (kx + 0.86, 40.4),
    (kx + 1.38, 43.72),
    (kx - 1.38, 43.72),
]

OUTLINE = style({'color': C('0d0b0b')})
RIM_SHADE = style(hvif.linear_gradient((41, 30.5), (39, 50), [
    (0.0, C('b4b9c3')), (0.3, C('727c8f')), (1.0, C('5a6476'))]))
RIM_LIGHT = style(hvif.linear_gradient((29, 31), (34, 50), [
    (0.0, C('f4f6fa')), (0.45, C('c3c9d4')), (1.0, C('96a0b0'))]))
FACE_EDGE = style({'color': C('0b3f86')})
FACE_SHADE = style(hvif.linear_gradient((40, 32), (38, 48), [
    (0.0, C('0a5ec6')), (0.5, C('054bac')), (1.0, C('003986'))]))
FACE_LIGHT = style(hvif.linear_gradient((31, 33), (34, 48), [
    (0.0, C('46d8fc')), (0.45, C('1a98f5')), (1.0, C('0b5cc4'))]))
KEYHOLE_RIM = style({'color': C('b9d0e8')})

rim_path, rim_left_path = path(rim), path(rim_left)
face_path, face_left_path = path(face), path(face_left)
keyhole_path = path(keyhole)
shape(OUTLINE, [rim_path], transformers=contour(1.4))
shape(RIM_SHADE, [rim_path])
shape(RIM_LIGHT, [rim_left_path])
shape(FACE_EDGE, [face_path], transformers=contour(0.7))
shape(FACE_SHADE, [face_path])
shape(FACE_LIGHT, [face_left_path])
shape(KEYHOLE_RIM, [keyhole_path], transformers=contour(0.7))
shape(OUTLINE, [keyhole_path])

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
