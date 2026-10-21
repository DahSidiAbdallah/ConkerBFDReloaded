import sys; sys.path.insert(0,sys.argv[2]); import conker_ui; conker_ui.use(sys.argv[3])
from PIL import Image
from conker_ui import button
P='/home/dah/ConkerRecomp/host/assets/'
base=Image.new('RGBA',(2560,1440),(0,0,0,255))
for line in open(P+'poster/layout.txt').read().splitlines()[1:]:
    f,x,y,w,h,*_=line.split(); base.alpha_composite(Image.open(P+'poster/'+f).convert('RGBA').resize((int(w),int(h))),(int(x),int(y)))
items=['START GAME','CONTROLS','SETTINGS','MODS','ROM: ORIGINAL','EXIT']; sel=0
right=2500; y=880
for i,t in enumerate(items):
    b=button(t,46,i==sel,seed=i+3,tilt=-3 if i%2 else 2)
    base.alpha_composite(b,(right-b.width,y)); y+=b.height-28
base.convert('RGB').resize((1280,720),Image.LANCZOS).save(sys.argv[1],quality=88)
base.crop((1600,700,2560,1440)).convert('RGB').resize((720,555),Image.LANCZOS).save(sys.argv[1].replace('.jpg','_zoom.jpg'),quality=88)
