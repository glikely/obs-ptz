"""Reads an equirectangular panorama for the pygfx renderer: a Radiance .hdr
(what Poly Haven's HDRIs are) or anything Pillow opens, as 8-bit sRGB rows of
RGBA, tonemapped and ready to put on the inside of a sphere.
"""

import numpy as np


def read_rgbe(path):
    """A Radiance .hdr as float32 linear RGB, shape (height, width, 3)"""
    with open(path, "rb") as f:
        data = f.read()
    pos, size = 0, None
    while True:
        end = data.index(b"\n", pos)
        line = data[pos:end].strip()
        pos = end + 1
        if line.startswith((b"-Y", b"+Y")):
            parts = line.split()
            if parts[0] != b"-Y" or parts[2] != b"+X":
                raise ValueError(f"{path}: only top-to-bottom, left-to-right .hdr files are supported")
            size = (int(parts[1]), int(parts[3]))
            break
    height, width = size
    rgbe = np.empty((height, width, 4), np.uint8)
    for y in range(height):
        if width < 8 or width > 32767 or data[pos:pos + 2] != b"\x02\x02" or data[pos + 2] & 0x80:
            # Flat, not run-length encoded
            rgbe[y] = np.frombuffer(data, np.uint8, width * 4, pos).reshape(width, 4)
            pos += width * 4
            continue
        pos += 4    # 2, 2 and the width again
        for channel in range(4):
            x = 0
            row = rgbe[y, :, channel]
            while x < width:
                count = data[pos]
                pos += 1
                if count > 128:     # a run of one value
                    count -= 128
                    row[x:x + count] = data[pos]
                    pos += 1
                else:               # that many different values
                    row[x:x + count] = np.frombuffer(data, np.uint8, count, pos)
                    pos += count
                x += count
    exponent = rgbe[..., 3].astype(np.int32) - (128 + 8)
    scale = np.where(rgbe[..., 3] > 0, np.ldexp(1.0, exponent), 0.0).astype(np.float32)
    return rgbe[..., :3].astype(np.float32) * scale[..., None]


def tonemap(linear, exposure=1.0):
    """float linear RGB to uint8 sRGB RGBA: ACES, as three.js's ACESFilmic does"""
    x = linear * exposure
    mapped = np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0)
    srgb = np.where(mapped <= 0.0031308, mapped * 12.92, 1.055 * np.power(mapped, 1 / 2.4) - 0.055)
    out = np.empty(srgb.shape[:2] + (4,), np.uint8)
    out[..., :3] = np.clip(srgb * 255.0 + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def load_panorama(path, exposure=1.0, max_width=4096):
    """The panorama in `path` as uint8 RGBA, no wider than max_width"""
    if path.lower().endswith(".hdr"):
        linear = read_rgbe(path)
        step = -(-linear.shape[1] // max_width)
        if step > 1:    # box filter down, in linear light
            h, w = (linear.shape[0] // step) * step, (linear.shape[1] // step) * step
            linear = linear[:h, :w].reshape(h // step, step, w // step, step, 3).mean(axis=(1, 3))
        return tonemap(linear, exposure)
    from PIL import Image
    image = Image.open(path).convert("RGB")
    if image.width > max_width:
        image = image.resize((max_width, image.height * max_width // image.width), Image.LANCZOS)
    rgba = np.empty((image.height, image.width, 4), np.uint8)
    rgba[..., :3], rgba[..., 3] = np.asarray(image), 255
    return rgba
