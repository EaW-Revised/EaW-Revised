"""CPU reference of the retail SceneBloom pass (docs/rendering.md#bloom).

Written from the traced pass, not from the viewer's shaders: an 8-bit stored
RGB frame goes through the bright pass into a quarter-size 8-bit target, four
diagonal 4-tap blurs and the add-smooth combine. Pure Python so the runtime
suites need nothing beyond Pillow; `combine` evaluates only the pixels asked
for, since the full frame is slow.
"""

import math

LUMINANCE = (0.299, 0.587, 0.114)
BLUR_ITERATIONS = 4


def target_extent(side: int) -> int:
    """A bloom target side: a quarter of the backbuffer side, truncated."""
    return int(0.25 * side)


def _unorm8(value: float) -> float:
    return math.floor(min(max(value, 0.0), 1.0) * 255.0 + 0.5) / 255.0


def _bilinear(fetch, width: int, height: int, x: float, y: float):
    """A LINEAR/CLAMP tap at texel-space (x, y), texel centres on integers."""
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = x - x0, y - y0

    def texel(tx, ty):
        return fetch(min(max(tx, 0), width - 1), min(max(ty, 0), height - 1))

    a, b = texel(x0, y0), texel(x0 + 1, y0)
    c, d = texel(x0, y0 + 1), texel(x0 + 1, y0 + 1)
    return tuple((a[i] * (1 - fx) + b[i] * fx) * (1 - fy) + (c[i] * (1 - fx) + d[i] * fx) * fy
                 for i in range(3))


def bloom_target(rgb: bytes, width: int, height: int, cutoff: float, size: float):
    """The last blur target, as rows of (r, g, b) in [0, 1] on 8-bit steps."""
    bw, bh = target_extent(width), target_extent(height)

    def frame(x, y):
        index = (y * width + x) * 3
        return rgb[index] / 255.0, rgb[index + 1] / 255.0, rgb[index + 2] / 255.0

    target = []
    for ty in range(bh):
        row = []
        for tx in range(bw):
            pixel = _bilinear(frame, width, height, (tx + 0.5) * width / bw - 0.5,
                              (ty + 0.5) * height / bh - 0.5)
            luminance = sum(weight * channel for weight, channel in zip(LUMINANCE, pixel))
            bright = pixel if luminance > cutoff else tuple(channel ** 5 for channel in pixel)
            row.append(tuple(_unorm8(channel) for channel in bright))
        target.append(row)
    for iteration in range(BLUR_ITERATIONS):
        offset = size * 0.5 * (1 + 2 * iteration)
        source = target

        def fetch(x, y, source=source):
            return source[y][x]

        target = []
        for ty in range(bh):
            row = []
            for tx in range(bw):
                taps = [_bilinear(fetch, bw, bh, tx + dx, ty + dy)
                        for dx, dy in ((offset, offset), (-offset, -offset),
                                       (offset, -offset), (-offset, offset))]
                row.append(tuple(_unorm8(sum(tap[i] for tap in taps) * 0.25) for i in range(3)))
            target.append(row)
    return target


def combine(rgb: bytes, width: int, height: int, target, strength: float, x: int, y: int):
    """The bloomed stored value of frame pixel (x, y), in [0, 1]."""
    bw, bh = len(target[0]), len(target)
    glow = _bilinear(lambda tx, ty: target[ty][tx], bw, bh,
                     (x + 0.5) * bw / width - 0.5, (y + 0.5) * bh / height - 0.5)
    index = (y * width + x) * 3
    result = []
    for channel in range(3):
        source = min(max(strength * glow[channel], 0.0), 1.0)
        destination = rgb[index + channel] / 255.0
        result.append(source + destination * (1.0 - source))
    return tuple(result)
