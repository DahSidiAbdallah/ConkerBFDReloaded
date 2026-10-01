"""Build an RT64 texture pack (.rtz, installed from the Mods menu) from upscaled PNGs named <rt64 hash>.png.
  make_pack.py PNG_DIR OUT.rtz --id conker_hd_icons --name "HD Icons" --description "..."
               [--author NAME ...] [--thumb PICTURE.png] [--license LICENSE.txt]
The pack's rt64.json maps each hash to its file; mod.json names it in the Mods menu, and
thumb.png (--thumb, square) is its picture there; --license adds a LICENSE.txt (credits and terms)."""
import argparse
import json
import os
import zipfile

ap = argparse.ArgumentParser()
ap.add_argument('png_dir')
ap.add_argument('out')
ap.add_argument('--id', required=True)
ap.add_argument('--name', required=True)
ap.add_argument('--description', default='')
ap.add_argument('--short', default='')
ap.add_argument('--version', default='1.0.0')
ap.add_argument('--folder', default='textures')
ap.add_argument('--author', action='append', default=None)
ap.add_argument('--thumb')
ap.add_argument('--license')
args = ap.parse_args()

hashes = sorted(n[:-4] for n in os.listdir(args.png_dir) if n.endswith('.png'))
database = {
    'configuration': {
        'autoPath': 'rt64',
        'configurationVersion': 3,
        'hashVersion': 5,
        # Small 2D pictures: load them with the game rather than streaming (no pop-in).
        'defaultOperation': 'preload',
        # Same pictures, more pixels: sampled at the original coordinates.
        'defaultShift': 'none',
    },
    'textures': [{'hashes': {'rt64': h}, 'path': f'{args.folder}/{h}'} for h in hashes],
}
manifest = {
    'game_id': 'conker',
    'id': args.id,
    'display_name': args.name,
    'description': args.description,
    'short_description': args.short or args.description,
    'version': args.version,
    'authors': args.author or ['dahmedvall95'],
    'minimum_recomp_version': '0.1.0',
}
with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('rt64.json', json.dumps(database, indent=4))
    z.writestr('mod.json', json.dumps(manifest, indent=4))
    if args.thumb:
        z.write(args.thumb, 'thumb.png')
    if args.license:
        z.write(args.license, 'LICENSE.txt')
    for h in hashes:
        z.write(os.path.join(args.png_dir, h + '.png'), f'{args.folder}/{h}.png')
print(len(hashes), 'textures ->', args.out)
