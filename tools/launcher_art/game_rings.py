# Cuts the game's own menu pill (the PLAY / QUIT ring of the save and pause menus) into parts the menus are built
# from: host/assets/menu/ring_<on|off>_<l|c|r>.png, left end, a middle to stretch (8 pixels, the same column repeated, so filtering doesn't pick up a
# neighbour), and the right end (the
# left end mirrored). "on" is the lit ring (the chosen item: green glow inside the ring), "off" the plain steel one.
# Taken from the HD Icons pack's pieces (textures by GameBeast92, modified, CC BY 4.0: see README), whose letters
# start past these columns.
#   .venv/bin/python tools/launcher_art/game_rings.py LIT.png UNLIT.png
#   (icons_work/mixed/2facfe3d50c12376.png, the lit "QU"; icons_work/mixed/9cbab0c6bdc643cd.png, the plain "QU")
import os, sys
from PIL import Image, ImageOps

out = os.path.join(os.path.dirname(__file__), '..', '..', 'host', 'assets', 'menu')
height = 96  # the parts' height; menus scale them to the button's
for state, path, cap in (('on', sys.argv[1], 112), ('off', sys.argv[2], 128)):
    src = Image.open(path).convert('RGBA').crop((0, 0, 384, 360))
    bbox = src.getbbox()
    src = src.crop((0, bbox[1], 384, bbox[3]))
    scale = height / src.height
    left = src.crop((0, 0, cap, src.height)).resize((round(cap * scale), height), Image.LANCZOS)
    middle = src.crop((cap, 0, cap + 1, src.height)).resize((1, height), Image.LANCZOS).resize((8, height))
    left.save(os.path.join(out, f'ring_{state}_l.png'), optimize=True)
    middle.save(os.path.join(out, f'ring_{state}_c.png'), optimize=True)
    ImageOps.mirror(left).save(os.path.join(out, f'ring_{state}_r.png'), optimize=True)
    print(state, left.size)

