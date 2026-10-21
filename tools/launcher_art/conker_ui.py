# Conker's own menu look: see-through pill rings (green when chosen, dark steel otherwise) with
# rounded green letters, and glossy titles with a gold rim (PAUSED, GAME1).
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
PALETTES={
 'green':dict(ring=((140,240,90),(20,110,10)), fill=((105,225,45),(25,130,10)), edge=((20,70,10),(5,30,4)), dim=((70,140,45),(30,80,20)), dimedge=((8,24,6),(2,10,2)), bar=(60,190,30)),
 'orange':dict(ring=((255,170,80),(190,40,0)), fill=((255,150,40),(205,40,0)), edge=((80,20,0),(35,6,0)), dim=((150,70,30),(85,30,10)), dimedge=((30,10,2),(12,4,0)), bar=(240,90,20)),
 # 'soft': easier on the eyes (the launcher's buttons): the chosen pill filled orange with light letters,
 # the others light letters in a steel ring.
 'soft':dict(ring=((255,192,125),(205,95,35)), fill=((255,250,240),(225,212,190)), edge=((70,30,8),(30,10,2)), dim=((238,230,214),(170,160,144)), dimedge=((22,18,14),(6,5,4)), bar=(200,86,26), pillfill=((224,114,42),(150,54,14))),
 'gold':dict(ring=((255,240,140),(170,110,10)), fill=((255,235,100),(210,130,10)), edge=((70,40,0),(30,15,0)), dim=((140,110,50),(80,60,20)), dimedge=((26,18,4),(10,6,0)), bar=(235,180,30), names=((255,255,255),(160,165,175))),
}
P_=PALETTES['green']
def use(name):
    global P_; P_=PALETTES[name]
FONT='/home/dah/ConkerRecomp/host/assets/LuckiestGuy-Regular.ttf'

def masks(text, size, strokes, seed=1, wobble=1.0):
    f=ImageFont.truetype(FONT,size); pad=int(size*0.35)
    b=f.getbbox(text); w=int(f.getlength(text))+pad*2; h=b[3]-b[1]+pad*2
    rng=np.random.default_rng(seed*7+len(text))
    wob=[(rng.uniform(-6,6)*wobble, rng.uniform(-0.06,0.06)*size*wobble) for _ in text]
    out=[]
    for st in strokes:
        m=Image.new('L',(w,h),0); x=pad
        for c,(rot,dy) in zip(text,wob):
            adv=f.getlength(c)
            if c!=' ':
                cs=int(size*1.7); cm=Image.new('L',(cs,cs),0)
                ImageDraw.Draw(cm).text((cs//2,cs//2),c,font=f,anchor='mm',fill=255,stroke_width=st,stroke_fill=255)
                cm=cm.rotate(rot,resample=Image.BICUBIC)
                m.paste(255,(int(x+adv/2-cs//2),int(h/2-cs//2+dy)),cm)
            x+=adv
        out.append(m)
    return out

def shade(mask, top, bottom, light=0.9, gloss=True):
    # a rounded, lit-from-above fill: colour gradient + bevel from the blurred mask + a shine band
    w,h=mask.size; a=np.asarray(mask,np.float32)/255
    g=np.linspace(0,1,h)[:,None,None]
    col=np.array(top,np.float32)*(1-g)+np.array(bottom,np.float32)*g
    r=max(2,h//28)
    bl=np.asarray(mask.filter(ImageFilter.GaussianBlur(r*1.5)),np.float32)/255
    ny=np.roll(bl,r,axis=0)-np.roll(bl,-r,axis=0)            # >0 on top edges
    col=col*(1+ny[...,None]*light)
    if gloss:
        band=np.exp(-((g[...,0]-0.32)/0.07)**2)*0.35
        col=col+255*band[...,None]*bl[...,None]
    col=np.clip(col,0,255).astype(np.uint8)
    im=Image.fromarray(col,'RGB').convert('RGBA'); im.putalpha(mask); return im

def letters(text, size, chosen, seed=1):
    s=max(2,size//16)
    edge,fill=masks(text,size,[s*2,0],seed)
    w,h=fill.size; out=Image.new('RGBA',(w,h),(0,0,0,0))
    if chosen: e=shade(edge,*P_['edge'],gloss=False); f=shade(fill,*P_['fill'])
    else:      e=shade(edge,*P_['dimedge'],gloss=False);   f=shade(fill,*P_['dim'],light=0.5)
    out.alpha_composite(e); out.alpha_composite(f); return out

def pill(w, h, chosen, tilt=-4):
    big=Image.new('L',(w,h),0); d=ImageDraw.Draw(big)
    t=max(6,h//7)
    d.rounded_rectangle((t//2,t//2,w-t//2,h-t//2),h//2,outline=255,width=t)
    if chosen and 'pillfill' in P_:
        inner=Image.new('L',(w,h),0); ImageDraw.Draw(inner).rounded_rectangle((t,t,w-t,h-t),h//2-t,fill=255)
        fillim=shade(inner,*P_['pillfill'],light=0.4)
    else: fillim=None
    if chosen: ring=shade(big,*P_['ring'])
    else:      ring=shade(big,(150,155,160),(20,22,26))
    rim=Image.new('L',(w,h),0); ImageDraw.Draw(rim).rounded_rectangle((t//2-2,t//2-2,w-t//2+2,h-t//2+2),h//2,outline=255,width=t+4)
    out=Image.new('RGBA',(w,h),(0,0,0,0)); out.paste((0,0,0,200),(0,0),rim)
    if fillim is not None: out.alpha_composite(fillim)
    out.alpha_composite(ring)
    return out

def button(text, size, chosen, seed=1, tilt=-4):
    L=letters(text,size,chosen,seed)
    pw=L.width+int(size*0.5); ph=int(size*1.75)
    P=pill(pw,ph,chosen)
    out=Image.new('RGBA',(pw+20,ph+20),(0,0,0,0))
    sh=Image.new('RGBA',out.size,(0,0,0,0)); sh.paste(P,(14,18)); a=sh.getchannel('A').point(lambda v:v*0.55).filter(ImageFilter.GaussianBlur(5))
    out.paste((0,0,0,255),(0,0),a)
    out.alpha_composite(P,(10,10)); out.alpha_composite(L,(10+(pw-L.width)//2,10+(ph-L.height)//2+int(size*0.04)))
    return out.rotate(tilt,resample=Image.BICUBIC,expand=True)

def title(text, size, colour='purple', seed=2):
    s=max(2,size//14)
    gold,dark,fill=masks(text,size,[s*2,s,0],seed,wobble=1.2)
    w,h=fill.size; out=Image.new('RGBA',(w,h),(0,0,0,0))
    sh=Image.new('RGBA',(w,h),(0,0,0,0)); sh.paste((0,0,0,220),(s,s*2),gold.filter(ImageFilter.GaussianBlur(s))); out.alpha_composite(sh)
    out.alpha_composite(shade(gold,(255,236,140),(150,100,20)))
    out.alpha_composite(shade(dark,(30,20,10),(10,6,4),gloss=False))
    top,bot={'purple':((120,100,150),(24,16,40)),'red':((255,70,50),(150,0,0))}[colour]
    out.alpha_composite(shade(fill,top,bot,light=1.1))
    return out

def gold(text, size, seed=1, wobble=0.5):
    # the lives number's look: shiny yellow-gold with a dark rim
    s=max(2,size//12)
    edge,fill=masks(text,size,[s*2,0],seed,wobble)
    w,h=fill.size; out=Image.new('RGBA',(w,h),(0,0,0,0))
    out.alpha_composite(shade(edge,(40,20,4),(10,5,0),gloss=False))
    out.alpha_composite(shade(fill,*P_.get('names',((255,240,110),(215,120,10)))))
    return out

def bubble(w, h, tail=True):
    # a speech bubble: white, rounded, thin black edge, small tail on the left
    out=Image.new('RGBA',(w+30,h+20),(0,0,0,0)); d=ImageDraw.Draw(out)
    if tail: d.polygon([(30,h*0.35),(0,h*0.55),(34,h*0.55)],fill=(0,0,0)); 
    d.rounded_rectangle((24,0,w+24,h),h//5 if h<200 else 40,fill=(0,0,0))
    d.rounded_rectangle((27,3,w+21,h-3),(h//5 if h<200 else 40)-3,fill=(255,255,255))
    if tail: d.polygon([(32,h*0.37),(5,h*0.54),(36,h*0.53)],fill=(255,255,255))
    return out
