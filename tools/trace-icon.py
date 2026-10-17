#!/usr/bin/env python3
"""Trace Burrow's logo artwork into the vector layers of its HVIF icon.

The artwork (resources/branding/burrow-logo.png) is posterized by region:
the black silhouette that forms the outlines, then the earth, grass and rocks
in four or five tones each, every tone stacked on the one below it. Each tone
is traced with potrace and written, in the 64 x 64 icon canvas, to
resources/branding/burrow-icon-layers.json. tools/make-icon.py adds the
shield, which is drawn by hand, and writes the HVIF.

Needs numpy, opencv-python-headless and potracer (pip), so it runs on the
development host only; the traced layers are committed.

    python3 tools/trace-icon.py [logo.png] [layers.json]
"""
import json
import os
import sys

import cv2
import numpy as np
import potrace
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, 'resources/branding/burrow-logo.png')
OUTPUT = os.path.join(ROOT, 'resources/branding/burrow-icon-layers.json')

# The artwork's content box (85, 141)-(1170, 1043) maps onto x 2..62 of the
# canvas, centred vertically. make-icon.py's shield uses the same mapping.
SCALE = 60.0 / 1085.0
ORIGIN = (85.0, 141.0)
OFFSET = (2.0, 7.0)

# Tracing settings: the base tone of a region keeps the outline detail, the
# tones above it are smoothed harder (they are shading, not outline).
BASE = {'sigma': 2.0, 'turd': 300, 'alphamax': 1.15, 'tolerance': 1.5}
DETAIL = {'sigma': 6.0, 'turd': 1500, 'alphamax': 1.33, 'tolerance': 4.0}
GRASS_DETAIL = dict(DETAIL, turd=400)
SILHOUETTE = {'sigma': 1.5, 'turd': 400, 'alphamax': 1.1, 'tolerance': 0.8}

# Value (HSV V) bands for each region's tones, darkest first.
EARTH_TONES = (0, 75, 125, 165, 210)
GRASS_TONES = (0, 120, 170, 215)
ROCK_TONES = (0, 100, 130, 165)


def fill_holes(mask):
    flood = mask.astype(np.uint8)
    seed = np.zeros((mask.shape[0] + 2, mask.shape[1] + 2), np.uint8)
    cv2.floodFill(flood, seed, (0, 0), 1)
    return mask | (flood == 0)


def largest(mask):
    count, labels, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
    return labels == 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))


def to_canvas(point):
    return [round((point[0] - ORIGIN[0]) * SCALE + OFFSET[0], 2),
            round((point[1] - ORIGIN[1]) * SCALE + OFFSET[1], 2)]


def trace(mask, sigma, turd, alphamax, tolerance):
    """Closed paths of HVIF points: [x, y] for corners, otherwise
    [x, y, in_x, in_y, out_x, out_y]."""
    smooth = cv2.GaussianBlur(mask.astype(np.float32), (0, 0), sigma) >= 0.5
    curves = potrace.Bitmap(~smooth).trace(turdsize=turd, alphamax=alphamax, opticurve=True,
                                            opttolerance=tolerance)
    paths = []
    for curve in curves:
        start = (curve.start_point.x, curve.start_point.y)
        nodes = [[start, start, start]]
        for segment in curve.segments:
            end = (segment.end_point.x, segment.end_point.y)
            if segment.is_corner:
                corner = (segment.c.x, segment.c.y)
                nodes += [[corner, corner, corner], [end, end, end]]
            else:
                nodes[-1][2] = (segment.c1.x, segment.c1.y)
                nodes.append([end, (segment.c2.x, segment.c2.y), end])
        # The last segment ends on the start point.
        nodes[0][1] = nodes.pop()[1]
        path = []
        for point, control_in, control_out in nodes:
            node = [to_canvas(point), to_canvas(control_in), to_canvas(control_out)]
            if path and path[-1][0] == node[0]:
                path[-1][2] = node[2]
            else:
                path.append(node)
        paths.append([n[0] if n[0] == n[1] == n[2] else n[0] + n[1] + n[2] for n in path])
    return paths


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else SOURCE
    output = sys.argv[2] if len(sys.argv) > 2 else OUTPUT
    image = np.array(Image.open(source).convert('RGBA'))
    rgb = image[:, :, :3]
    opaque = image[:, :, 3] >= 128
    hsv = cv2.cvtColor(rgb, cv2.COLOR_RGB2HSV).astype(np.int32)
    hue, saturation, value = hsv[..., 0], hsv[..., 1], hsv[..., 2]

    outline = opaque & (value < 40)
    grass = opaque & ~outline & (hue >= 25) & (hue <= 60) & (saturation > 60)
    blue = opaque & ~outline & (hue >= 90) & (hue <= 125) & (saturation > 90)
    grey = opaque & ~outline & ~grass & ~blue & (saturation < 60)
    earth = opaque & ~outline & ~grass & ~blue & ~grey
    # The shield is drawn by hand; keep its rim out of the rocks and earth.
    shield = fill_holes(largest(grey) | largest(blue))
    shield = fill_holes(cv2.morphologyEx(shield.astype(np.uint8), cv2.MORPH_CLOSE,
                                         np.ones((9, 9), np.uint8)).astype(bool))
    rocks = grey & ~shield
    earth &= ~shield

    layers = []

    def add(name, mask, color_mask, settings):
        color = rgb[color_mask].mean(0)
        layers.append({'name': name, 'color': '%02x%02x%02x' % tuple(int(round(c)) for c in color),
                       'paths': trace(mask, **settings)})

    add('silhouette', fill_holes(opaque), opaque & (value < 40), SILHOUETTE)
    for region, mask, tones, detail in (('earth', earth, EARTH_TONES, DETAIL),
                                        ('grass', grass, GRASS_TONES, GRASS_DETAIL),
                                        ('rock', rocks, ROCK_TONES, DETAIL)):
        for index, low in enumerate(tones):
            high = tones[index + 1] if index + 1 < len(tones) else 256
            add('%s%d' % (region, index), mask & (value >= low),
                mask & (value >= low) & (value < high), BASE if index == 0 else detail)

    with open(output, 'w') as f:
        f.write('{"source": "burrow-logo.png", "layers": [\n')
        for index, layer in enumerate(layers):
            f.write('  {"name": "%s", "color": "%s", "paths": [\n' % (layer['name'], layer['color']))
            f.write(',\n'.join('    ' + json.dumps(p, separators=(',', ':')) for p in layer['paths']))
            f.write('\n  ]}%s\n' % (',' if index + 1 < len(layers) else ''))
        f.write(']}\n')
    points = sum(len(p) for layer in layers for p in layer['paths'])
    print('%s: %d layers, %d points' % (output, len(layers), points))


if __name__ == '__main__':
    main()
