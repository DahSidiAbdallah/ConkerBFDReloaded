"""Upscale 2D textures (decode_dump.py's PNGs) 4x with Real-ESRGAN (realesrgan-ncnn-vulkan, anime model).
  upscale_icons.py IN_DIR OUT_DIR [--exe C:/path/realesrgan-ncnn-vulkan.exe] [--model realesrgan-x4plus-anime]
Colour and transparency are upscaled apart: the colour of transparent pixels is first filled
in from their visible neighbours (it's often black or junk, which would bleed in as dark halos),
and the transparency is upscaled on its own. Each image is padded with its edge pixels before
and trimmed after, so its borders stay clean. Soft, see-through images (smoke, glows: much of
their transparency partial) are only scaled smoothly: the AI gives them hard, jagged edges."""
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter

SCALE = 4
PAD = 4


def fill_transparent(rgba):
    a = rgba[..., 3] > 0
    rgb = rgba[..., :3].astype(np.float32)
    if a.all() or not a.any():
        return rgba[..., :3]
    filled = rgb.copy()
    known = a.copy()
    # Grow the visible colours outwards into the transparent pixels, a pixel at a time.
    for _ in range(max(rgba.shape[:2])):
        if known.all():
            break
        acc = np.zeros_like(filled)
        cnt = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            f = np.roll(np.roll(filled, dy, 0), dx, 1)
            acc += f * k[..., None]
            cnt += k
        new = (~known) & (cnt > 0)
        filled[new] = acc[new] / cnt[new][:, None]
        known = known | new
    return np.clip(filled, 0, 255).astype(np.uint8)


def to_windows(path):
    path = os.path.abspath(path)
    if path.startswith('/mnt/'):
        return path[5].upper() + ':' + path[6:].replace('/', '\\')
    return path


def main():
    src, dst = sys.argv[1], sys.argv[2]
    exe = sys.argv[sys.argv.index('--exe') + 1] if '--exe' in sys.argv else '/mnt/c/ConkerRecompWin/tools/realesrgan/realesrgan-ncnn-vulkan.exe'
    model = sys.argv[sys.argv.index('--model') + 1] if '--model' in sys.argv else 'realesrgan-x4plus-anime'
    os.makedirs(dst, exist_ok=True)
    # The upscaler runs on Windows: work in a folder it can reach.
    work = tempfile.mkdtemp(prefix='icons_', dir='/mnt/c/ConkerRecompWin/tools/realesrgan')
    rgb_in, a_in, rgb_out, a_out = (os.path.join(work, n) for n in ('rgb_in', 'a_in', 'rgb_out', 'a_out'))
    for d in (rgb_in, a_in, rgb_out, a_out):
        os.makedirs(d)
    names = [n for n in sorted(os.listdir(src)) if n.endswith('.png')]
    sizes = {}
    for n in names:
        im = np.asarray(Image.open(os.path.join(src, n)).convert('RGBA'))
        sizes[n] = im.shape[:2]
        rgb = fill_transparent(im)
        rgb = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='edge')
        alpha = np.pad(im[..., 3], ((PAD, PAD), (PAD, PAD)), mode='edge')
        Image.fromarray(rgb, 'RGB').save(os.path.join(rgb_in, n))
        Image.fromarray(np.stack([alpha] * 3, -1), 'RGB').save(os.path.join(a_in, n))
    for i, o in ((rgb_in, rgb_out), (a_in, a_out)):
        subprocess.run([exe, '-i', to_windows(i), '-o', to_windows(o), '-n', model, '-s', str(SCALE), '-f', 'png'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=os.path.dirname(exe))
    for n in names:
        h, w = sizes[n]
        orig = Image.open(os.path.join(src, n)).convert('RGBA')
        a = np.asarray(orig)[..., 3]
        if ((a > 16) & (a < 239)).mean() > 0.10:
            orig.resize((w * SCALE, h * SCALE), Image.LANCZOS).save(os.path.join(dst, n))
            continue
        rgb = np.asarray(Image.open(os.path.join(rgb_out, n)).convert('RGB'))
        alpha = np.asarray(Image.open(os.path.join(a_out, n)).convert('L'))
        c = PAD * SCALE
        rgb, alpha = rgb[c:c + h * SCALE, c:c + w * SCALE], alpha[c:c + h * SCALE, c:c + w * SCALE]
        Image.fromarray(np.dstack([rgb, alpha]), 'RGBA').save(os.path.join(dst, n))
    shutil.rmtree(work, ignore_errors=True)
    print(len(names), 'upscaled')


if __name__ == '__main__':
    main()
