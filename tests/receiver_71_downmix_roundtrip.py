#!/usr/bin/env python3
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
import pathlib, struct, subprocess, sys, tempfile

ENC, FFMPEG = sys.argv[1:3]
FRAMES = 2048
CH = 8
V = 0x700000

class BR:
    def __init__(self, data, bit=0): self.d=data; self.p=bit
    def get(self,n):
        v=0
        for _ in range(n):
            if self.p >= len(self.d)*8: raise RuntimeError('truncated bitstream')
            v=(v<<1)|((self.d[self.p>>3]>>(7-(self.p&7)))&1); self.p+=1
        return v

def s24(v):
    if v < 0: v += 1<<24
    return bytes((v&255,(v>>8)&255,(v>>16)&255))

def run(cmd): subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

def parse_set(data, off, maskbits, hier_ofs):
    r=BR(data,off*8); start=r.p
    size=r.get(10)+1; nch=r.get(4)+1; full=r.get(nch)
    depth=r.get(5)+1; storage=r.get(5)+1; freq=r.get(4); r.get(2); r.get(2)
    primary=r.get(1); dmix=r.get(1); embedded=0
    if dmix:
        embedded=r.get(1)
        if primary: r.get(3)
    hier=r.get(1)
    scales=[]; coeffs=[]
    if dmix and not primary:
        for _ in range(hier_ofs):
            scales.append(r.get(9)); coeffs.append([r.get(9) for _ in range(nch)])
    if r.get(1)!=1: raise RuntimeError('channel mask disabled')
    mask=r.get(maskbits)
    decor=r.get(1)
    order=[]; decor_coeff=[]
    if decor and nch>1:
        order_bits=1 if nch<=2 else 2 if nch<=4 else 3
        order=[r.get(order_bits) for _ in range(nch)]
        for _ in range(nch//2):
            present=r.get(1)
            decor_coeff.append(r.get(7) if present else None)
    adaptive=[r.get(4) for _ in range(nch)]
    fixed=[r.get(2) if a==0 else None for a in adaptive]
    return dict(size=size,nch=nch,full=full,depth=depth,storage=storage,freq=freq,
                primary=primary,dmix=dmix,embedded=embedded,hier=hier,scales=scales,
                coeffs=coeffs,mask=mask,decor=decor,order=order,decor_coeff=decor_coeff,
                adaptive=adaptive,fixed=fixed,parsed_bits=r.p-start)

def inspect(path):
    d=path.read_bytes(); ex=d.find(bytes.fromhex('64582025')); xl=d.find(bytes.fromhex('41a29547'))
    if ex < 0 or xl < 0: raise RuntimeError('missing EXSS/XLL')
    r=BR(d,ex*8)
    assert r.get(32)==0x64582025; r.get(8); assert r.get(2)==0; assert r.get(1)==0
    hsize=r.get(8)+1; r.get(16)
    assert hsize==32 and xl==ex+32, (hsize,xl-ex)
    assert r.get(1)==1; assert r.get(2)==2; r.get(3); assert r.get(1)==0
    assert r.get(3)==0 and r.get(3)==0 and r.get(1)==1 and r.get(8)==1 and r.get(1)==0
    r.get(16); ds=r.p; desc=r.get(9)+1
    assert desc==15; assert r.get(3)==0 and r.get(1)==0 and r.get(1)==0 and r.get(1)==0
    assert r.get(5)+1==24 and r.get(4)==12 and r.get(8)+1==8
    assert r.get(1)==1 and r.get(1)==0 and r.get(1)==1
    assert r.get(1)==1 and r.get(2)==2 and r.get(12)==0x84b
    assert r.get(3)==0 and r.get(1)==0 and r.get(1)==0
    assert r.get(2)==0 and r.get(12)==0x201
    r.get(16); assert r.get(1)==1 and r.get(4)==8 and r.get(5)+1==1 and r.get(1)==0
    assert r.get(16)==0 and r.get(3)==0
    r.p=ds+desc*8
    assert r.get(1)==1 and r.get(2)==0 and r.get(3)==0
    assert r.get(8)==0

    x=BR(d,xl*8); assert x.get(32)==0x41a29547; assert x.get(4)==0
    common=x.get(8)+1; fbits=x.get(5)+1; frame_bytes=x.get(fbits)+1
    assert x.get(4)+1==2
    grid=x.get(4); x.get(4); segbits=x.get(5)+1; x.get(2)
    scalable=x.get(1); maskbits=x.get(5)+1
    fixed_lsb=x.get(4) if scalable else None
    assert maskbits==11
    # The supplied receiver-compatible 7.1 reference never uses the old fixed
    # 16/14-bit hierarchical size fields.  Both fields use the minimum needed
    # width with a seven-bit floor, and scalable-LSB syntax stays enabled even
    # when this frame has an empty LSB section.
    assert scalable==1 and fixed_lsb==0
    assert fbits==max(7,(frame_bytes-1).bit_length()), (fbits,frame_bytes)
    p=parse_set(d,xl+common,maskbits,0)
    s=parse_set(d,xl+common+p['size'],maskbits,6)
    nav=BR(d,(xl+common+p['size']+s['size'])*8)
    segment_sizes=[]
    for _ in range(1<<grid):
        segment_sizes.extend((nav.get(segbits)+1,nav.get(segbits)+1))
    assert segbits==max(7,max(v-1 for v in segment_sizes).bit_length()), (segbits,segment_sizes)
    assert p['nch']==6 and p['full']==0x3f and p['mask']==0x627 and p['primary']==1 and p['hier']==1 and p['storage']==24
    assert s['nch']==2 and s['full']==3 and s['mask']==0x180 and s['primary']==0 and s['dmix']==1 and s['embedded']==1 and s['hier']==1
    assert s['scales']==[473]*6, s['scales']
    zero=256
    expected=[[zero,zero] for _ in range(6)]; expected[4][0]=473; expected[5][1]=473
    assert s['coeffs']==expected, s['coeffs']
    # The supplied reference encoder uses fixed order 0 exclusively. Keep that
    # convention in both the six-channel primary and Lsr/Rsr extension set;
    # channels that need prediction must use adaptive prediction instead.
    for label, chset in (('primary', p), ('rear', s)):
        bad=[v for v in chset['fixed'] if v not in (None,0)]
        assert not bad, (label,chset['adaptive'],chset['fixed'])

with tempfile.TemporaryDirectory(prefix='libhdma-71-rx-') as td:
    td=pathlib.Path(td); raw=td/'peak.pcm'; out=td/'out.dts'; full=td/'full.pcm'; dm=td/'dm.pcm'
    # Keep a nonzero front stage while BL BR SL SR are all loud and
    # same-polarity, forcing the side+rear intermediate beyond signed 24-bit.
    F=0x300000
    frame=[F,-F,F//2,-F//2,V,V,V,V]
    raw.write_bytes(b''.join(s24(v) for _ in range(FRAMES) for v in frame))
    run([ENC,str(raw),'--raw-pcm','--sample-rate','48000','--channels','8','--bit-depth','24',
         '--channel-layout','7.1','--lossless','--threads','1','--simd','none','--no-progress',
         '--overwrite','-o',str(out)])
    inspect(out)
    run([FFMPEG,'-hide_banner','-nostdin','-v','error','-xerror','-f','dts','-i',str(out),
         '-f','s32le','-c:a','pcm_s32le','-y',str(full)])
    vals=struct.unpack('<%di'%(full.stat().st_size//4), full.read_bytes())
    expected=[v<<8 for v in frame]
    for n in range(FRAMES):
        if list(vals[n*8:n*8+8]) != expected:
            raise RuntimeError(f'7.1 mismatch at sample {n}: {vals[n*8:n*8+8]} != {expected}')
    run([FFMPEG,'-hide_banner','-nostdin','-v','error','-xerror','-f','dts','-downmix','5.1(side)',
         '-i',str(out),'-f','s32le','-c:a','pcm_s32le','-y',str(dm)])
    dvals=struct.unpack('<%di'%(dm.stat().st_size//4), dm.read_bytes())
    # The observed DCA/XLL hierarchy applies code 473 (-3 dB) to all six primary
    # rows.  No surround energy enters FL/FR: they are only globally scaled.
    def mul15(v,c): return (v*c+16384)>>15
    expected_front=[mul15(F,23170)<<8,mul15(-F,23170)<<8,
                    mul15(F//2,23170)<<8,mul15(-F//2,23170)<<8]
    for n in range(FRAMES):
        got=list(dvals[n*6:n*6+4])
        if got != expected_front:
            raise RuntimeError(f'5.1 primary scale/routing mismatch at sample {n}: {got} != {expected_front}')
    left=[dvals[i] for i in range(4,len(dvals),6)]; right=[dvals[i] for i in range(5,len(dvals),6)]
    if not left or min(left)<=0 or min(right)<=0:
        raise RuntimeError('5.1 embedded surround downmix wrapped negative')
    # Loud same-polarity side+rear is expected to saturate at the 24-bit output
    # ceiling rather than wrap; that is why official tools expose saturation checks.
    ceiling=0x7fffff<<8
    if min(left)!=ceiling or max(left)!=ceiling or min(right)!=ceiling or max(right)!=ceiling:
        raise RuntimeError(f'unexpected saturated surround range L={min(left)}..{max(left)} R={min(right)}..{max(right)}')
print('PASS 7.1 receiver hierarchy: code-473 primary scale, side+rear -> surrounds only, 24-bit storage, peak-safe')
