# Makes the launcher's button pictures in the game's own menu style (conker_ui.py, the soft palette):
# host/assets/menu/<name>_on.png (chosen) and <name>_off.png, at the poster's scale (2560x1440).
#   .venv/bin/python tools/launcher_art/make_buttons.py
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import conker_ui
conker_ui.use('soft')
out = os.path.join(os.path.dirname(__file__), '..', '..', 'host', 'assets', 'menu')
os.makedirs(out, exist_ok=True)
labels = {
    'start_game': 'START GAME', 'load_rom': 'LOAD ROM', 'controls': 'CONTROLS', 'settings': 'SETTINGS',
    'mods': 'MODS', 'exit': 'EXIT', 'change_rom': 'CHANGE ROM',
    'rom_original': 'ROM: ORIGINAL', 'rom_uncensored': 'ROM: UNCENSORED', 'rom_french': 'ROM: FRENCH',
    'rom_german': 'ROM: GERMAN', 'rom_russian': 'ROM: RUSSIAN', 'rom_modified': 'ROM: MODIFIED',
}
order = ['start_game', 'controls', 'settings', 'mods', 'rom_original', 'exit']
for i, (name, text) in enumerate(labels.items()):
    row = order.index(name) if name in order else {'load_rom': 0, 'change_rom': 4}.get(name, 4)
    tilt = -3 if row % 2 else 2
    for state in ('on', 'off'):
        im = conker_ui.button(text, 46, state == 'on', seed=row + 3, tilt=tilt)
        im = im.crop(im.getbbox())
        im.save(os.path.join(out, f'{name}_{state}.png'), optimize=True)
print('made', len(labels) * 2, 'pictures in', os.path.normpath(out))
