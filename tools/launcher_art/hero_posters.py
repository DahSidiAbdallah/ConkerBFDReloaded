# Launcher posters in a single-scene, movie-poster style: one real game frame (snaps/ab captures), zoomed so
# the character sits to one side, the other side darkened for the logo (assets/poster/logo.png, its own piece
# so it lands with the opening). Writes host/assets/poster_<id>/ (full.png, shade.png, layout.txt).
#   .venv/bin/python tools/launcher_art/hero_posters.py [preview.jpg]
import sys, shutil, os
import numpy as np
from PIL import Image, ImageFilter, ImageEnhance
R = '/home/dah/ConkerRecomp/'; A = R + 'host/assets/'; S = '/mnt/c/ConkerRecompWin/snaps/ab/'
W, H = 1920, 1080

def frame(name):   # the game area of a 1618x947 window capture
    return Image.open(S + name + '.png').convert('RGB').crop((9, 38, 1609, 937))

def grade(im):
    im = ImageEnhance.Color(im).enhance(1.12)
    return im.filter(ImageFilter.UnsharpMask(2, 60, 2))

def place(im, zoom, fx, fy):   # zoom, then put the frame's point (fx, fy) (0..1) at the poster's middle
    w, h = round(W * zoom), round(H * zoom)
    big = im.resize((w, h), Image.LANCZOS)
    x = round(fx * w - W / 2); y = round(fy * h - H / 2)
    x = max(0, min(w - W, x)); y = max(0, min(h - H, y))
    return big.crop((x, y, x + W, y + H))

def darken(im, side, amount=0.75, width=0.55):   # a soft dark band on one side for the logo
    a = np.asarray(im, np.float32)
    x = np.linspace(0, 1, W)[None, :, None]
    t = np.clip((width - (x if side == 'left' else 1 - x)) / width, 0, 1) ** 1.5
    return Image.fromarray((a * (1 - amount * t)).astype(np.uint8))

def without_menu_text(im):   # the bar's chapter screen without its "CHAPTERS" title and "INSERT RUMBLE PAK"
    import cv2
    a = cv2.cvtColor(np.asarray(im), cv2.COLOR_RGB2BGR)
    b, g, r = [a[..., i].astype(int) for i in range(3)]
    mask = np.zeros(a.shape[:2], np.uint8)
    top = np.zeros_like(mask); top[50:185, 580:1030] = 1
    mask |= ((((b > r + 35) & (b > 90)) | ((b > 150) & (g > 150) & (r > 130))) & (top > 0)).astype(np.uint8) * 255
    bottom = np.zeros_like(mask); bottom[770:860, 500:1120] = 1
    mask |= ((r > g + 50) & (r > 110) & (bottom > 0)).astype(np.uint8) * 255
    out = cv2.inpaint(a, cv2.dilate(mask, np.ones((13, 13), np.uint8)), 9, cv2.INPAINT_TELEA)
    return Image.fromarray(cv2.cvtColor(out, cv2.COLOR_BGR2RGB))

def darken_top(im, amount=0.6, height=0.5):   # a soft dark band at the top for the logo
    a = np.asarray(im, np.float32)
    y = np.linspace(0, 1, H)[:, None, None]
    t = np.clip((height - y) / height, 0, 1) ** 1.5
    return Image.fromarray((a * (1 - amount * t)).astype(np.uint8))

def triptych(names, cuts=(0, 660, 1280, W)):   # three tall slices with slanted black cuts
    # (cuts starting right of 0: the band left of them is a blurred dark copy of the middle scene, for the logo)
    from PIL import ImageDraw
    out = ImageEnhance.Brightness(grade(frame(names[1][0])).resize((W, H)).filter(ImageFilter.GaussianBlur(30))).enhance(0.3)
    slant = 90; gap = 14
    for i, (n, fx) in enumerate(names):
        im = place(grade(frame(n)), 1.0, fx, 0.5)
        x0, x1 = cuts[i], cuts[i + 1]
        poly = [(x0 + (slant if x0 else 0) + gap, 0), (x1 + (slant if i < 2 else 0) - gap, 0),
                (x1 - gap, H), (x0 + gap, H)]
        mask = Image.new('L', (W, H)); ImageDraw.Draw(mask).polygon(poly, fill=255)
        shift = round((x0 + x1) / 2 - fx * W)   # the frame's point fx in the slice's middle
        src = Image.new('RGB', (W, H)); src.paste(im, (shift, 0))
        out.paste(src, (0, 0), mask)
    return out

new = np.array(Image.open(A + 'poster/logo.png').convert('RGBA'))
ny, nx = np.nonzero(new[..., 3] > 200); nb = (nx.min(), ny.min(), nx.max(), ny.max())

def logo_line(cx, cy, letters_h):   # on the 2560x1440 poster
    s = letters_h / (nb[3] - nb[1])
    lx = cx - (nb[0] + nb[2]) / 2 * s; ly = cy - (nb[1] + nb[3]) / 2 * s
    return f"poster/logo.png {round(lx)} {round(ly)} {round(new.shape[1] * s)} {round(new.shape[0] * s)} logo\n"

posters = {
    # id: (picture, logo centre x, y, letters height on 2560x1440)
    'poster_king':    (lambda: darken(place(grade(frame('hq_newgame/t90.5')), 1.15, 0.36, 0.40), 'left', 0.85, 0.45), 470, 660, 900),
    'poster_chainsaw': (lambda: darken(place(grade(frame('hq_intro/t29.75')), 1.15, 0.47, 0.45), 'right', 0.7, 0.5), 1930, 470, 760),
    'poster_dollar':  (lambda: darken(place(grade(frame('hq_cash/t65.0')), 1.2, 0.46, 0.40), 'left'), 560, 640, 980),
    'poster_panther': (lambda: darken(place(grade(frame('hq_newgame/t384.0')), 1.1, 0.40, 0.45), 'left', 0.7), 560, 640, 980),
    'poster_berri':   (lambda: darken(place(grade(frame('hq_newgame/t191')), 1.1, 0.40, 0.45), 'right', 0.85, 0.55), 1930, 470, 760),
    # Gregg at the bar, from the chapter screen (save user_2026-10-17_war.eep, GAME1 -> CHAPTERS); the logo covers where the title was.
    'poster_gregg':   (lambda: darken_top(place(grade(without_menu_text(frame('chap1/t54.5'))), 1.0, 0.5, 0.5)), 1300, 380, 600),
    'poster_three':   (lambda: darken(triptych([('hq_intro/t29.75', 0.36), ('hq_newgame/t191', 0.27), ('hq_newgame/t384.0', 0.58)], (560, 1010, 1460, W)), 'left', 0.0), 470, 640, 880),
}


previews = []
for pid, (make, cx, cy, lh) in posters.items():
    os.makedirs(A + pid, exist_ok=True)
    im = make(); im.save(A + pid + '/full.png', optimize=True)
    shutil.copy(A + 'poster/shade.png', A + pid + '/shade.png')
    line = logo_line(cx, cy, lh)
    open(A + pid + '/layout.txt', 'w').write("2560 1440\nfull.png 0 0 2560 1440 full\nshade.png 1460 440 1100 1000 shade\n" + line)
    if len(sys.argv) > 1:
        p = im.resize((2560, 1440)).convert('RGBA')
        _, x, y, w, h, _ = line.split()
        lg = Image.open(A + 'poster/logo.png').convert('RGBA').resize((int(w), int(h)))
        p.paste(lg, (int(x), int(y)), lg)
        previews.append(p.convert('RGB').resize((640, 360)))
if previews:
    sheet = Image.new('RGB', (1280, 360 * 3))
    for i, p in enumerate(previews): sheet.paste(p, ((i % 2) * 640, (i // 2) * 360))
    sheet.save(sys.argv[1], quality=82)
