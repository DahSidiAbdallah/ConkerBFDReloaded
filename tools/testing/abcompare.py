"""Side-by-side comparison of two abrun.sh runs, with a zoomed-in strip and a measure of
how much of the picture changed.
  abcompare.py A B TIME OUT.png [label A] [label B] [crop x0,y0,x1,y1 as fractions]
Prints the share of pixels that differ noticeably (any channel by more than 12/255)."""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

root = '/mnt/c/ConkerRecompWin/snaps/ab'
a_name, b_name, t, out = sys.argv[1:5]
la = sys.argv[5] if len(sys.argv) > 5 else a_name
lb = sys.argv[6] if len(sys.argv) > 6 else b_name
crop = [float(v) for v in sys.argv[7].split(',')] if len(sys.argv) > 7 else [0.35, 0.35, 0.6, 0.6]

def load(name):
    im = Image.open(f'{root}/{name}/t{t}.png').convert('RGB')
    w, h = im.size
    return im.crop((8, 31, w - 8, h - 8))  # drop the window frame and title bar

a, b = load(a_name), load(b_name)
b = b.resize(a.size)
diff = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).max(axis=2)
changed_all = (diff > 12).mean() * 100

W = 800
scale = W / a.width
H = int(a.height * scale)
w, h = a.size
box = (int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h))
changed = (diff[box[1]:box[3], box[0]:box[2]] > 12).mean() * 100
print(f'{a_name} vs {b_name} t{t}: {changed:.1f}% of pixels in the box differ ({changed_all:.1f}% overall)')
zw = W; zh = int((box[3] - box[1]) * zw / (box[2] - box[0]))
sheet = Image.new('RGB', (2 * W + 10, H + zh + 70), (40, 40, 40))
try:
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 26)
except OSError:
    font = ImageFont.load_default()
d = ImageDraw.Draw(sheet)
for i, (im, label) in enumerate([(a, la), (b, lb)]):
    x = i * (W + 10)
    sheet.paste(im.resize((W, H), Image.LANCZOS), (x, 40))
    d.text((x + 8, 6), label, fill=(255, 220, 90), font=font)
    rx0, ry0 = x + box[0] * scale, 40 + box[1] * scale
    d.rectangle((rx0, ry0, x + box[2] * scale, 40 + box[3] * scale), outline=(255, 220, 90), width=3)
    sheet.paste(im.crop(box).resize((zw, zh), Image.NEAREST), (x, H + 60))
d.text((8, H + 42 - 20), f'zoomed (yellow box)  -  {changed:.1f}% of its pixels differ', fill=(200, 200, 200), font=font)
sheet.save(out)
