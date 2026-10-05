# Regenerates input/*.png (same code that produced the committed inputs; numpy default_rng seeds fixed)
import numpy as np
from PIL import Image
N=256
y,x=np.mgrid[0:N,0:N].astype(np.float64)
r=x; g=y; b=255-(x+y)/2
img=np.stack([r,g,b,np.full_like(r,255)],-1)
Image.fromarray(np.clip(np.rint(img),0,255).astype(np.uint8),'RGBA').save('input/grad_rgb.png')
rng=np.random.default_rng(12345)
def vnoise(cells,seed):
    rr=np.random.default_rng(seed)
    grid=rr.random((cells+1,cells+1))
    gx=x/(N/cells); gy=y/(N/cells)
    x0=np.floor(gx).astype(int); y0=np.floor(gy).astype(int)
    fx=gx-x0; fy=gy-y0
    fx=fx*fx*(3-2*fx); fy=fy*fy*(3-2*fy)
    a=grid[y0,x0]; bb=grid[y0,x0+1]; c=grid[y0+1,x0]; d=grid[y0+1,x0+1]
    return (a*(1-fx)+bb*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy
def fbm(seed):
    v=np.zeros((N,N)); amp=1; tot=0
    for i,c in enumerate([4,8,16,32,64]):
        v+=amp*vnoise(c,seed+i); tot+=amp; amp*=0.55
    return v/tot
R=fbm(1); G=fbm(100); B=fbm(200)
img=np.stack([R,G,B],-1)*255
img[40:90,150:220]=[30,40,60]
d=np.hypot(x-80,y-170)
img[d<40]=[240,200,60]
img[::32,:]=[255,255,255]
img+= rng.normal(0,6,img.shape)
img=np.clip(np.rint(img),0,255).astype(np.uint8)
a=np.full((N,N,1),255,np.uint8)
Image.fromarray(np.concatenate([img,a],-1),'RGBA').save('input/photo.png')
col=np.stack([x, 255-y, np.full_like(x,128)],-1)
alpha=np.zeros((N,N))
d=np.hypot(x-96,y-96)
alpha=np.maximum(alpha, np.clip((70-d)/10,0,1))
alpha[160:,:]=np.maximum(alpha[160:,:], (x[160:,:]/255.0))
alpha[20:60,170:236]=1.0
alpha[30:50,180:226]=0.5
img=np.concatenate([col, alpha[...,None]*255],-1)
Image.fromarray(np.clip(np.rint(img),0,255).astype(np.uint8),'RGBA').save('input/alpha_edges.png')
h=x/256.0*6
def hsv2rgb(h,s,v):
    i=np.floor(h).astype(int)%6; f=h-np.floor(h)
    p=v*(1-s); q=v*(1-s*f); t=v*(1-s*(1-f))
    r=np.choose(i,[v,q,p,p,t,v]); g=np.choose(i,[t,v,v,q,p,p]); b=np.choose(i,[p,p,t,v,v,q])
    return r,g,b
s=np.clip(y/128.0,0,1); v=np.where(y<128,1.0,1.0-(y-128)/256.0)
r,g,b=hsv2rgb(h,s,v)
alpha=np.where(y<192,255.0,255.0-(y-192)*3.0)
img=np.stack([r*255,g*255,b*255,alpha],-1)
Image.fromarray(np.clip(np.rint(img),0,255).astype(np.uint8),'RGBA').save('input/blend_top.png')
