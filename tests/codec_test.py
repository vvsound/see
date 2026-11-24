import struct,subprocess,random,io,math,os
import numpy as np
from PIL import Image
random.seed(1)
def packbits(row):
    out=bytearray();i=0;n=len(row)
    while i<n:
        j=i
        while j+1<n and row[j+1]==row[i] and j-i<127: j+=1
        if j>i:
            out+=bytes([(1-(j-i+1))&0xFF,row[i]]);i=j+1
        else:
            k=i
            while k+1<n and row[k+1]!=row[k] and k-i<127: k+=1
            k=min(k+1,n)
            if k-i>128:k=i+128
            out+=bytes([k-i-1])+bytes(row[i:k]);i=k
    return bytes(out)
def psd(planes,W,H,depth,mode,ver=1,comp=0,neg_layers=False,pal=b''):
    ch=len(planes)
    hdr=b'8BPS'+struct.pack('>H',ver)+b'\0'*6+struct.pack('>HIIHH',ch,H,W,depth,mode)
    hdr+=struct.pack('>I',len(pal))+pal
    hdr+=struct.pack('>I',0)  # resources
    if neg_layers:
        li=struct.pack('>h',-1)+b'\0\0'
        li=li+b'\0'*((-len(li))%2)
        layer=(struct.pack('>I' if ver==1 else '>Q',len(li))+li)
        hdr+=struct.pack('>I' if ver==1 else '>Q',len(layer))+layer
    else:
        hdr+=struct.pack('>I' if ver==1 else '>Q',0)
    body=struct.pack('>H',comp)
    rb=(W*depth+7)//8
    rows=[[bytes(p[y*rb:(y+1)*rb]) for y in range(H)] for p in planes]
    if comp==0:
        for c in rows:
            for r in c: body+=r
    else:
        cs='>H' if ver==1 else '>I'
        enc=[[packbits(r) for r in c] for c in rows]
        for c in enc:
            for r in c: body+=struct.pack(cs,len(r))
        for c in enc:
            for r in c: body+=r
    return hdr+body
def run(kind,data,name):
    open(name,'wb').write(data)
    o=subprocess.run(['/tmp/codec_dump',kind,name],capture_output=True).stdout
    nl=o.index(b'\n');head=o[:nl].decode()
    if head.startswith('ERR') or head=='FAIL': return head,None
    w,h,b=map(int,head.split());a=np.frombuffer(o[nl+1:],np.uint8).reshape(h,w,4)
    return (w,h,b),a
ok=True
def check(name,cond):
    global ok
    print(('PASS ' if cond else 'FAIL ')+name); ok&=bool(cond)
W,H=7,5
rgb=np.random.randint(0,256,(H,W,3),np.uint8)
def planes8(a): return [a[...,i].tobytes() for i in range(a.shape[2])]
for comp in (0,1):
    for ver in (1,2):
        info,a=run('psd',psd(planes8(rgb),W,H,8,3,ver,comp),'t.psd')
        check(f'rgb8 comp{comp} ver{ver}', a is not None and info==(W,H,24) and (a[...,2]==rgb[...,0]).all() and (a[...,1]==rgb[...,1]).all() and (a[...,0]==rgb[...,2]).all() and (a[...,3]==255).all())
# flat-colour rows to exercise RLE repeats
fl=np.zeros((H,W,3),np.uint8);fl[:,:4]=[10,20,30];fl[:,4:]=[200,100,50]
info,a=run('psd',psd(planes8(fl),W,H,8,3,1,1),'t.psd'); check('rgb8 rle runs',a is not None and (a[...,2]==fl[...,0]).all() and (a[...,0]==fl[...,2]).all())
# 16-bit
v16=np.random.randint(0,65536,(H,W,3)).astype('>u2')
info,a=run('psd',psd([v16[...,i].tobytes() for i in range(3)],W,H,16,3,1,0),'t.psd')
exp=((v16.astype(np.uint32)*255+32767)//65535).astype(np.uint8)
check('rgb16',a is not None and (a[...,2]==exp[...,0]).all() and (a[...,0]==exp[...,2]).all())
# gray
g=np.random.randint(0,256,(H,W),np.uint8)
info,a=run('psd',psd([g.tobytes()],W,H,8,1),'t.psd'); check('gray',a is not None and (a[...,0]==g).all() and (a[...,1]==g).all())
# alpha with negative layer count
al=np.random.randint(0,256,(H,W),np.uint8)
info,a=run('psd',psd(planes8(rgb)+[al.tobytes()],W,H,8,3,1,1,neg_layers=True),'t.psd')
exp_b=((rgb[...,2].astype(np.uint32)*al+127)//255)
check('rgb+merged alpha (premult)',a is not None and info==(W,H,32) and (a[...,3]==al).all() and (a[...,0]==exp_b).all())
# extra channel but NOT transparency -> ignored
info,a=run('psd',psd(planes8(rgb)+[al.tobytes()],W,H,8,3,1,0,neg_layers=False),'t.psd')
check('extra alpha channel ignored',a is not None and info==(W,H,24) and (a[...,3]==255).all() and (a[...,2]==rgb[...,0]).all())
# CMYK (inverted storage)
cm=np.random.randint(0,256,(H,W,4),np.uint8)
info,a=run('psd',psd(planes8(cm),W,H,8,4,1,1),'t.psd')
er=(cm[...,0].astype(np.uint32)*cm[...,3]//255)
check('cmyk',a is not None and (a[...,2]==er).all())
# bitmap 1-bit: 1=black
rb=(W+7)//8
bits=np.random.randint(0,2,(H,W),np.uint8)
packed=np.packbits(bits,axis=1).tobytes()
info,a=run('psd',psd([packed],W,H,1,0),'t.psd')
check('bitmap',a is not None and ((a[...,0]==0)==(bits==1)).all())
# indexed
pal=bytes(range(256))+bytes(255-i for i in range(256))+bytes((i*3)%256 for i in range(256))
idx=np.random.randint(0,256,(H,W),np.uint8)
info,a=run('psd',psd([idx.tobytes()],W,H,8,2,1,0,pal=pal),'t.psd')
check('indexed',a is not None and (a[...,2]==idx).all() and (a[...,1]==255-idx.astype(int)).all())
# 32-bit float gray linear -> sRGB
f=np.random.rand(H,W).astype('>f4')
info,a=run('psd',psd([f.tobytes()],W,H,32,1,1,0),'t.psd')
def enc(v): 
    v=np.clip(v.astype(np.float64),0,1);g=np.where(v<=0.0031308,12.92*v,1.055*v**(1/2.4)-0.055);return np.floor(g*255+0.5).astype(np.uint8)
check('gray32 float',a is not None and np.abs(a[...,0].astype(int)-enc(f)).max()<=1)
# truncated / garbage must not crash
d=psd(planes8(rgb),W,H,8,3,1,1)
for cut in (10,30,50,len(d)-5):
    info,a=run('psd',d[:cut],'t.psd'); check(f'truncated {cut} no crash',info is not None)
# HDR
def hdr_file(w,h,rle,sy='-',sx='+'):
    img=np.random.rand(h,w,3)*4
    out=io.BytesIO();out.write(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n'+f'{sy}Y {h} {sx}X {w}\n'.encode())
    rg=np.zeros((h,w,4),np.uint8)
    for y in range(h):
        for x in range(w):
            r,g,b=img[y,x];m=max(r,g,b);mm,e=math.frexp(m);k=mm*256/m
            rg[y,x]=[int(r*k),int(g*k),int(b*k),e+128]
    for y in range(h):
        if rle:
            out.write(bytes([2,2,w>>8,w&255]))
            for c in range(4):
                row=bytes(rg[y,:,c]);i=0
                while i<w:
                    j=i
                    while j+1<w and row[j+1]==row[i] and j-i<126:j+=1
                    if j-i>=2: out.write(bytes([128+(j-i+1),row[i]]));i=j+1
                    else:
                        k=min(i+100,w);out.write(bytes([k-i])+row[i:k]);i=k
        else:
            out.write(rg[y].tobytes())
    return out.getvalue(),rg
def ref_tm(rg):
    e=rg[...,3].astype(int);lin=np.where(e[...,None]==0,0,(rg[...,:3]+0.5)*np.ldexp(1.0,e-136)[...,None])
    return lin
for rle in (True,False):
    data,rg=hdr_file(40,12,rle)
    info,a=run('hdr',data,'t.hdr')
    ok_=a is not None and info==(40,12,24)
    if ok_:
        lin=ref_tm(rg);k=max(1,40//256);s=lin[::k,::k];lum=0.2126*s[...,0]+0.7152*s[...,1]+0.0722*s[...,2]
        key=math.exp(np.mean(np.log(lum[lum>0]+1e-4)));sc=min(max(0.18/key,0.05),50)
        x=lin*sc;y=np.clip((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0,1)
        ok_=np.abs(a[...,2].astype(int)-enc(y[...,0])).max()<=1 and np.abs(a[...,0].astype(int)-enc(y[...,2])).max()<=1
    check(f'hdr rle={rle}',ok_)
data,rg=hdr_file(16,6,True,'+','-')
info,a=run('hdr',data,'t.hdr');d0,rg0=None,None
check('hdr flip header parsed',a is not None and info==(16,6,24))
info,a=run('hdr',b'#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 2 +X 2\n','t.hdr');check('hdr xyze rejected',a is None)
# RAW preview
def jpg(w,h,q):
    b=io.BytesIO();Image.fromarray(np.random.randint(0,256,(h,w,3),np.uint8)).save(b,'JPEG',quality=q);return b.getvalue()
small,big=jpg(160,120,80),jpg(640,480,85)
prog=io.BytesIO();Image.fromarray(np.random.randint(0,256,(900,700,3),np.uint8)).save(prog,'JPEG',progressive=True,quality=70);prog=prog.getvalue()
tiff=b'II*\0'+struct.pack('<I',8)+struct.pack('<H',1)+struct.pack('<HHII',0x112,3,1,6)+b'\0\0\0\0'
raw=tiff+b'\0'*50+small+b'\xff\xd8\xff'+b'junk'*20+b'\0'*30+big+b'\0'*100+prog+b'\0'*10
open('t.raw','wb').write(raw)
o=subprocess.run(['/tmp/codec_dump','raw','t.raw'],capture_output=True).stdout.decode().split()
off,ln,ori=int(o[0]),int(o[1]),int(o[2])
check('raw picks largest (progressive 900x700)',raw[off:off+ln]==prog and ori==6)
open('t2.raw','wb').write(b'\0'*30+small+b'\0'*5+big)
o=subprocess.run(['/tmp/codec_dump','raw','t2.raw'],capture_output=True).stdout.decode().split()
check('raw picks big, orientation default 1',raw is not None and int(o[2])==1 and Image.open(io.BytesIO(open('t2.raw','rb').read()[int(o[0]):int(o[0])+int(o[1])])).size==(640,480))

# ---------------- 解压:gzip / zlib(含动态哈夫曼、存储块、固定哈夫曼)
import zlib,gzip
def dump(kind,data,name):
    open(name,'wb').write(data)
    r=subprocess.run(['/tmp/codec_dump',kind,name],capture_output=True)
    return r.stdout if r.returncode==0 else None
rnd=bytes(random.randrange(256) for _ in range(5000))
txt=(b'hello xview '*500)+rnd+(bytes(range(256))*40)
for lvl in (0,1,6,9):
    check(f'gunzip level {lvl}',dump('gz',gzip.compress(txt,compresslevel=lvl),'t.gz')==txt)
    check(f'zlib level {lvl}',dump('zlib',zlib.compress(txt,lvl),'t.z')==txt)
cz=zlib.compressobj(6,zlib.DEFLATED,15,9,zlib.Z_FIXED);fx=cz.compress(b'abcabcabc'*30)+cz.flush()
check('zlib fixed huffman',dump('zlib',fx,'t.z')==b'abcabcabc'*30)
g=gzip.compress(txt);check('gunzip truncated rejected',dump('gz',g[:len(g)//2],'t.gz') is None)
check('gunzip garbage rejected',dump('gz',b'\x1f\x8b\x08\x00'+bytes(100),'t.gz') is None)

# ---------------- EXR
def exr_file(W,H,chans,comp,vals,ptype=1,x0=0,y0=0,raw_store=False):
    # chans: 名字列表(要按字母序);vals: dict 名字->float32 数组 (H,W)
    def attr(n,t,d): return n.encode()+b'\0'+t.encode()+b'\0'+struct.pack('<i',len(d))+d
    cl=b''
    for c in sorted(chans):
        cl+=c.encode()+b'\0'+struct.pack('<iBBBBii',ptype,0,0,0,0,1,1)
    cl+=b'\0'
    h=b'\x76\x2f\x31\x01'+struct.pack('<I',2)
    h+=attr('channels','chlist',cl)+attr('compression','compression',bytes([comp]))
    h+=attr('dataWindow','box2i',struct.pack('<iiii',x0,y0,x0+W-1,y0+H-1))
    h+=attr('displayWindow','box2i',struct.pack('<iiii',x0,y0,x0+W-1,y0+H-1))
    h+=attr('lineOrder','lineOrder',b'\0')+attr('pixelAspectRatio','float',struct.pack('<f',1))
    h+=attr('screenWindowCenter','v2f',struct.pack('<ff',0,0))+attr('screenWindowWidth','float',struct.pack('<f',1))+b'\0'
    lpb=16 if comp==3 else 1
    nb=(H+lpb-1)//lpb
    chunks=[]
    for b in range(nb):
        rows=range(b*lpb,min(H,(b+1)*lpb))
        buf=b''
        for y in rows:
            for c in sorted(chans):
                a=vals[c][y]
                buf+=(a.astype('<f2') if ptype==1 else a.astype('<f4')).tobytes()
        if comp==0: data=buf
        else:
            ev=buf[0::2]+buf[1::2]
            pr=bytearray(ev)
            for i in range(len(pr)-1,0,-1): pr[i]=(pr[i]-pr[i-1]+128)&255
            pr=bytes(pr)
            if comp==1:
                o=bytearray();i=0
                while i<len(pr):
                    j=i
                    while j+1<len(pr) and pr[j+1]==pr[i] and j-i<126: j+=1
                    if j>i: o+=bytes([j-i])+bytes([pr[i]]);i=j+1
                    else:
                        k=i
                        while k+1<len(pr) and pr[k+1]!=pr[k] and k-i<126: k+=1
                        k=min(k+1,len(pr),i+127)
                        o+=bytes([(-(k-i))&255])+pr[i:k];i=k
                data=bytes(o)
            else: data=zlib.compress(pr)
            if raw_store or len(data)>=len(buf): data=buf
        chunks.append(struct.pack('<ii',y0+b*lpb,len(data))+data)
    base=len(h)+8*nb;offs=[];cur=base
    for c in chunks: offs.append(cur);cur+=len(c)
    return h+b''.join(struct.pack('<Q',o) for o in offs)+b''.join(chunks)
def exr_expect(R,G,B,A=None):
    lin=np.stack([R,G,B],-1).astype(np.float64)
    k=max(1,max(R.shape)//256);s=lin[::k,::k];lum=0.2126*s[...,0]+0.7152*s[...,1]+0.0722*s[...,2]
    lm=lum[lum>0];key=math.exp(np.mean(np.log(lm+1e-4))) if lm.size else 1
    sc=min(max(0.18/key,0.05),50) if lm.size else 1
    x=np.maximum(lin*sc,0);y=np.clip((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14),0,1)
    return enc(y)
np.random.seed(3)
W,H=37,21
base=(np.random.rand(H,W)*3).astype(np.float32)
vals={'R':base,'G':np.flipud(base).copy(),'B':(base*0.3).astype(np.float32)}
for ptype in (1,2):
    for comp in (0,1,2,3):
        v={k:(a.astype(np.float16).astype(np.float32) if ptype==1 else a) for k,a in vals.items()}
        info,a=run('exr',exr_file(W,H,['R','G','B'],comp,v,ptype),'t.exr')
        e=exr_expect(v['R'],v['G'],v['B']) if a is not None else None
        good=a is not None and info==(W,H,24) and np.abs(a[...,2].astype(int)-e[...,0]).max()<=1 and np.abs(a[...,1].astype(int)-e[...,1]).max()<=1 and np.abs(a[...,0].astype(int)-e[...,2]).max()<=1 and (a[...,3]==255).all()
        check(f'exr ptype={ptype} comp={comp}',good)
# 压缩后反而变大 -> 文件里存的是原始数据
v={k:a.astype(np.float16).astype(np.float32) for k,a in vals.items()}
info,a=run('exr',exr_file(W,H,['R','G','B'],3,v,1,raw_store=True),'t.exr');check('exr stored-raw block under zip',a is not None and info==(W,H,24))
# alpha
v4=dict(v);v4['A']=np.clip(np.random.rand(H,W),0,1).astype(np.float16).astype(np.float32)
info,a=run('exr',exr_file(W,H,['R','G','B','A'],2,v4,1),'t.exr')
check('exr alpha -> 32 bit, premultiplied clamp',a is not None and info==(W,H,32) and (np.abs(a[...,3].astype(int)-np.round(v4['A']*255)).max()<=1) and (a[...,:3]<=a[...,3:4]).all())
# 单通道 Y
info,a=run('exr',exr_file(W,H,['Y'],3,{'Y':v['R']},1),'t.exr')
check('exr Y only -> grey',a is not None and (a[...,0]==a[...,1]).all() and (a[...,1]==a[...,2]).all())
# 数据窗口不从 0 开始
info,a=run('exr',exr_file(W,H,['R','G','B'],3,v,1,x0=-5,y0=7),'t.exr');check('exr offset data window',a is not None and info==(W,H,24))
# 不支持 / 损坏
d=exr_file(W,H,['R','G','B'],3,v,1)
info,a=run('exr',d.replace(b'compression\0compression\0\x01\0\0\0\x03',b'compression\0compression\0\x01\0\0\0\x04'),'t.exr');check('exr PIZ rejected with message',a is None and 'PIZ' in info)
info,a=run('exr',b'\x76\x2f\x31\x01'+struct.pack('<I',2|0x200)+d[8:],'t.exr');check('exr tiled rejected',a is None)
info,a=run('exr',d[:len(d)//2],'t.exr');check('exr truncated: no crash (blank or error)',info is not None)
bad=bytearray(d)
for _ in range(60): bad[random.randrange(len(bad))]=random.randrange(256)
info,a=run('exr',bytes(bad),'t.exr');check('exr corrupted: no crash',info is not None)

print('ALL OK' if ok else 'SOME FAILED')
