# Makes the launcher's button pictures from the game's own menu pill (game_rings.py's parts: the steel ring, lit green
# with a glow inside when chosen) and the game's font: host/assets/menu/<name>_on.png (chosen: bright letters glowing,
# the ring lit) and <name>_off.png (the plain ring, green letters). As in the game's menus, nothing fills the ring.
#   .venv/bin/python tools/launcher_art/make_buttons.py
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

here = os.path.dirname(__file__)
out = os.path.join(here, '..', '..', 'host', 'assets', 'menu')
font = ImageFont.truetype(os.path.join(here, '..', '..', 'host', 'assets', 'LuckiestGuy-Regular.ttf'), 150)
labels = {
    'start_game': 'START GAME', 'load_rom': 'LOAD ROM', 'controls': 'CONTROLS', 'settings': 'SETTINGS',
    'mods': 'MODS', 'exit': 'EXIT', 'change_rom': 'CHANGE ROM',
    'rom_original': 'ROM: ORIGINAL', 'rom_uncensored': 'ROM: UNCENSORED', 'rom_french': 'ROM: FRENCH',
    'rom_german': 'ROM: GERMAN', 'rom_russian': 'ROM: RUSSIAN', 'rom_modified': 'ROM: MODIFIED',
}
work_height = 360  # drawn at this height, saved at the pictures' size
picture_height = 100
on_letters, on_outline, on_glow = (205, 255, 95, 255), (15, 60, 10, 255), (120, 255, 40, 210)
off_letters, off_outline = (70, 150, 70, 255), (8, 25, 8, 255)


def ring(state, width):
    parts = [Image.open(os.path.join(out, f'ring_{state}_{p}.png')).convert('RGBA') for p in 'lcr']
    scale = work_height / parts[0].height
    left, middle, right = (p.resize((max(1, round(p.width * scale)), work_height), Image.LANCZOS) for p in parts)
    out_im = Image.new('RGBA', (width, work_height))
    out_im.paste(left, (0, 0))
    out_im.paste(middle.resize((width - left.width - right.width, work_height)), (left.width, 0))
    out_im.paste(right, (width - right.width, 0))
    return out_im


def button(text, on):
    measure = ImageDraw.Draw(Image.new('RGBA', (1, 1)))
    text_width = measure.textlength(text, font=font)
    im = ring('on' if on else 'off', int(text_width + 250))
    layer = Image.new('RGBA', im.size, (0, 0, 0, 0))
    x, y = (im.width - text_width) / 2, im.height / 2 - 80
    if on:
        glow = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(glow).text((x, y), text, font=font, fill=on_glow)
        glow = glow.filter(ImageFilter.GaussianBlur(14))
        layer = Image.alpha_composite(Image.alpha_composite(layer, glow), glow)
    ImageDraw.Draw(layer).text((x, y), text, font=font, fill=on_letters if on else off_letters,
                               stroke_width=5, stroke_fill=on_outline if on else off_outline)
    im = Image.alpha_composite(im, layer)
    return im.resize((round(im.width * picture_height / im.height), picture_height), Image.LANCZOS)


for name, text in labels.items():
    for state in ('on', 'off'):
        button(text, state == 'on').save(os.path.join(out, f'{name}_{state}.png'), optimize=True)
print('made', len(labels) * 2, 'pictures in', os.path.normpath(out))
