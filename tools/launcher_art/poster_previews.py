# Small pictures of the launcher posters for the Launcher Poster setting's description:
# host/assets/menu/poster_preview_<n>.png, n = the setting's order (frontend.cpp poster_folders); and each
# poster's base.png, a blurred dark copy shown under it from the launcher's first frame.
#   .venv/bin/python tools/launcher_art/poster_previews.py
from PIL import Image, ImageFilter, ImageEnhance
A = '/home/dah/ConkerRecomp/host/assets/'
folders = ['poster', 'poster_old', 'poster_king', 'poster_chainsaw', 'poster_dollar', 'poster_panther', 'poster_three',
           'poster_birdy', 'poster_sunflower', 'poster_gregg', 'poster_berri']
for n, folder in enumerate(folders):
    lines = open(A + folder + '/layout.txt').read().split('\n')
    pw, ph = map(int, lines[0].split())
    c = Image.new('RGBA', (pw, ph), (0, 0, 0, 255))
    for line in lines[1:]:
        if not line.strip(): continue
        f, x, y, w, h, name = line.split()
        if name in ('shade', 'base'): continue
        im = Image.open(A + (f if '/' in f else folder + '/' + f)).convert('RGBA').resize((int(float(w)), int(float(h))))
        c.paste(im, (int(float(x)), int(float(y))), im)
    c.convert('RGB').resize((480, 270), Image.LANCZOS).save(A + 'menu/poster_preview_%d.png' % n, optimize=True)
    base = ImageEnhance.Brightness(c.convert('RGB').resize((320, 180), Image.LANCZOS).filter(ImageFilter.GaussianBlur(5))).enhance(0.45)
    base.save(A + folder + '/base.png', optimize=True)
    if not any(l.startswith('base.png') for l in lines):
        lines.insert(1, 'base.png 0 0 %d %d base' % (pw, ph))
        open(A + folder + '/layout.txt', 'w').write('\n'.join(lines))
