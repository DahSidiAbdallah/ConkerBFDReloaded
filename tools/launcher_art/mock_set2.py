import sys; sys.path.insert(0,sys.argv[2]); import conker_ui; conker_ui.use(sys.argv[3])
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from conker_ui import button, title, letters, gold, bubble
P='/home/dah/ConkerRecomp/host/assets/'
W,H=1618,909
bg=Image.new('RGBA',(2560,1440),(0,0,0,255))
for line in open(P+'poster/layout.txt').read().splitlines()[1:]:
    f,x,y,w,h,*_=line.split()
    if f.startswith('panel'): bg.alpha_composite(Image.open(P+'poster/'+f).convert('RGBA').resize((int(w),int(h))),(int(x),int(y)))
img=Image.alpha_composite(bg.resize((W,H)).filter(ImageFilter.GaussianBlur(9)),Image.new('RGBA',(W,H),(0,0,0,150)))
d=ImageDraw.Draw(img)
LG=lambda s: ImageFont.truetype(P+'LuckiestGuy-Regular.ttf',s)
IN=lambda s: ImageFont.truetype(P+'InterVariable.ttf',s)
cream=(250,240,220); dim=(170,170,160)
# tab row: small pills, the open one green
tabs=['CONKER','ACCESSIBILITY','GENERAL','GRAPHICS','CONTROLS','SOUND','MODS']; sel=2
x=40
for i,t in enumerate(tabs):
    b=button(t,27,i==sel,seed=i+20,tilt=0); img.alpha_composite(b,(x,20)); x+=b.width-6
t=title('GENERAL',64,'purple'); img.alpha_composite(t,((W-t.width)//2-190,95))
d=ImageDraw.Draw(img)
img.alpha_composite(gold('X',40,seed=3,wobble=0),(W-90,22)); d=ImageDraw.Draw(img)
opts=[('Joystick Deadzone','slider',0.06,'6%'),('Background Input Mode','choice',['OFF','ON'],1),('Rumble: Strength','slider',0.25,'25%'),
      ('Rumble: Motor','choice',['SMALL','LARGE','BOTH'],0),('Free Camera','choice',['OFF','RIGHT STICK','RIGHT STICK AND MOUSE'],2),
      ('Button Prompts','choice',['AUTOMATIC','CONTROLLER','KEYBOARD AND MOUSE'],0),('Camera: Turning Speed','slider',0.2,'100%')]
y=215; hl=4
for i,(name,kind,v,extra) in enumerate(opts):
    if i==hl:
        L=letters(name.upper(),30,True,seed=40+i); img.alpha_composite(L,(60-12,y-14))
    else:
        G=gold(name.upper(),26,seed=40+i); img.alpha_composite(G,(60-9,y-9))
    yy=y+42
    if kind=='slider':
        G=gold(extra,18,seed=80+i,wobble=0); img.alpha_composite(G,(60-6,yy-5)); d=ImageDraw.Draw(img)
        sx=130; d.rounded_rectangle((sx-2,yy+4,sx+302,yy+18),7,fill=(0,0,0)); d.rounded_rectangle((sx,yy+6,sx+300,yy+16),5,fill=(70,74,80))
        d.rounded_rectangle((sx,yy+6,sx+int(300*v),yy+16),5,fill=conker_ui.P_['bar']); cx=sx+int(300*v)
        d.ellipse((cx-11,yy,cx+11,yy+22),fill=(200,205,210),outline=(0,0,0),width=3)
    else:
        xx=50
        for j,c in enumerate(v):
            b=button(c,20,j==extra,seed=j+60+i,tilt=0); img.alpha_composite(b,(xx,yy-16)); xx+=b.width-4
    d=ImageDraw.Draw(img)
    y+=92
t2=title('FREE CAMERA',40,'red',seed=5); img.alpha_composite(t2,(860,200))
B=bubble(620,190); img.alpha_composite(B,(820,280)); d=ImageDraw.Draw(img)
for k,l in enumerate(['Turn the camera yourself with the right stick,','or the right stick and the mouse.','','Off keeps the game\'s own camera.']):
    d.text((880,305+k*34),l,font=LG(22),fill=(0,0,0))
img.alpha_composite(gold('CLOSE:  B / ESC',16,seed=9,wobble=0),(32,H-48))
img.convert('RGB').save(sys.argv[1],quality=88)
