# The settings menu's backdrop: the launcher poster, blurred and darkened.
#   .venv/bin/python tools/launcher_art/make_settings_bg.py host/assets/menu/settings_bg.png
import sys
from PIL import Image, ImageFilter
P='/home/dah/ConkerRecomp/host/assets/poster/'
W,H=960,540
bg=Image.new('RGBA',(2560,1440),(0,0,0,255))
for line in open(P+'layout.txt').read().splitlines()[1:]:
    f,x,y,w,h,*_=line.split()
    if f.startswith('panel'): bg.alpha_composite(Image.open(P+f).convert('RGBA').resize((int(w),int(h))),(int(x),int(y)))
img=bg.resize((W,H),Image.LANCZOS).filter(ImageFilter.GaussianBlur(6))
img=Image.alpha_composite(img,Image.new('RGBA',(W,H),(0,0,0,160)))
img.convert('RGB').save(sys.argv[1],optimize=True)
