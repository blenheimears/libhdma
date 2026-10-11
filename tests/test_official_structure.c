/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Validate the compatibility structure observed in the supplied official
 * 5.1/24-bit/48-kHz DCA + XLL sample. This is intentionally independent of
 * the encoder implementation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { const uint8_t *p; size_t bytes, bit; int error; } Bits;
static uint32_t get(Bits *b,unsigned n)
{
    uint32_t v=0;
    if(n>32 || b->error || b->bit+n>b->bytes*8U) { b->error=1; return 0; }
    while(n--) { size_t p=b->bit++; v=(v<<1)|((b->p[p>>3]>>(7-(p&7)))&1U); }
    return v;
}
static unsigned needbits(uint32_t v) { unsigned n=1; while(v>>n)n++; return n; }
static int fail(const char *what) { fprintf(stderr,"official-structure: %s\n",what); return 1; }
int main(int argc,char **argv)
{
    if(argc!=2) return fail("usage FILE");
    FILE *f=fopen(argv[1],"rb"); if(!f)return fail("open");
    if(fseek(f,0,SEEK_END))return fail("seek"); long n=ftell(f);
    if(n<2048 || n>16*1024*1024 || fseek(f,0,SEEK_SET))return fail("size");
    uint8_t *d=malloc((size_t)n); if(!d)return fail("memory");
    if(fread(d,1,(size_t)n,f)!=(size_t)n){free(d);return fail("read");} fclose(f);
    Bits c={d,(size_t)n,0,0};
    if(get(&c,32)!=0x7ffe8001U)return fail("core sync");
    get(&c,1);get(&c,5);unsigned crc=get(&c,1);get(&c,7);
    unsigned core=get(&c,14)+1U; get(&c,6);get(&c,4);get(&c,5);
    get(&c,1);get(&c,1);get(&c,1);get(&c,1);get(&c,1);get(&c,3);get(&c,1);
    if(get(&c,1)!=1U)return fail("core sub-subframe sync flag");
    get(&c,2);get(&c,1); if(crc)get(&c,16); get(&c,1);get(&c,4);
    if(get(&c,2)!=1U)return fail("core copy-history code");
    if(get(&c,3)!=6U)return fail("core 24-bit resolution code");
    size_t exoff=(core+3U)&~3U;
    Bits e={d,(size_t)n,exoff*8U,0};
    if(get(&e,32)!=0x64582025U)return fail("EXSS sync");
    get(&e,8); if(get(&e,2)!=0 || get(&e,1)!=0)return fail("EXSS index/header form");
    unsigned hsize=get(&e,8)+1U; unsigned exsize=get(&e,16)+1U;
    if(hsize!=32U)return fail("EXSS header is not 32 bytes");
    if(get(&e,1)!=1U || get(&e,2)!=2U)return fail("EXSS static/clock fields");
    get(&e,3); if(get(&e,1))return fail("unexpected timecode");
    if(get(&e,3)!=0 || get(&e,3)!=0 || get(&e,1)!=1 || get(&e,8)!=1 || get(&e,1)!=0)
        return fail("EXSS presentation/asset fields");
    unsigned asset=get(&e,16)+1U;
    size_t ds=e.bit; if(get(&e,9)+1U!=14U)return fail("asset descriptor size");
    if(get(&e,3)!=0 || get(&e,1) || get(&e,1) || get(&e,1))return fail("asset descriptor flags");
    if(get(&e,5)+1U!=24U || get(&e,4)!=12U || get(&e,8)+1U!=6U)return fail("asset PCM fields");
    if(get(&e,1)!=1U || get(&e,1)!=0U || get(&e,1)!=1U)return fail("asset speaker flags");
    if(get(&e,2)!=0U || get(&e,4)!=15U || get(&e,3)!=0U)return fail("asset speaker mask");
    if(get(&e,1)||get(&e,1))return fail("asset DRC/dialog flags");
    if(get(&e,2)!=0U || get(&e,12)!=0x201U)return fail("explicit CORE+XLL mask");
    unsigned xllsize=get(&e,16)+1U;
    if(get(&e,1)!=1U || get(&e,4)!=0U || get(&e,5)+1U!=1U || get(&e,1)!=0U ||
       get(&e,16)!=0U || get(&e,3)!=0U)return fail("XLL sync metadata");
    if(e.bit-ds!=106U)return fail("asset descriptor parsed length");
    e.bit=ds+14U*8U;
    if(get(&e,1)!=1U || get(&e,2)!=0U || get(&e,3)!=0U || get(&e,16)!=0U)
        return fail("core association/reserved padding");
    get(&e,16); if(e.error)return fail("EXSS header truncation");
    if(asset!=xllsize || exsize!=32U+xllsize)return fail("EXSS/asset lengths");
    size_t xo=exoff+32U; Bits x={d,(size_t)n,xo*8U,0};
    if(get(&x,32)!=0x41a29547U || get(&x,4)!=0U)return fail("XLL sync/version");
    unsigned common=get(&x,8)+1U,fbits=get(&x,5)+1U,framesize=get(&x,fbits)+1U;
    if(framesize!=xllsize || needbits(framesize-1U)!=fbits)return fail("minimum XLL frame-size field");
    if(get(&x,4)!=0U)return fail("XLL channel-set count");
    unsigned grid=get(&x,4),slog=get(&x,4),sbits=get(&x,5)+1U;
    if(grid<1U || grid>4U || slog>8U || grid+slog!=9U)return fail("invalid XLL segment grid: 48-kHz segments must be <=256 samples");
    get(&x,2); if(get(&x,1)!=0U)return fail("scalable LSB mode enabled");
    if(get(&x,5)+1U!=6U)return fail("XLL speaker-mask width");
    x.bit=(xo+common)*8U;
    unsigned sub=get(&x,10)+1U,channels=get(&x,4)+1U;
    unsigned residual_mask=get(&x,channels);
    /* XLL convention: 1 = full PCM, 0 = a core residual. The hidden
     * analysis prime is not transmitted: frame one must be independent. */
    if(channels!=6U || residual_mask!=63U)
        return fail("first frame references unavailable core history");
    get(&x,5);get(&x,5);get(&x,4);get(&x,2);get(&x,2);get(&x,1);get(&x,1);get(&x,1);
    if(get(&x,1)!=1U || get(&x,6)!=63U)return fail("XLL channel mask");
    unsigned decor=get(&x,1);
    if(decor) {
        unsigned obits=3; for(unsigned ch=0;ch<channels;ch++)get(&x,obits);
        for(unsigned pair=0;pair<channels/2;pair++)if(get(&x,1))get(&x,7);
    }
    for(unsigned ch=0;ch<channels;ch++)if(get(&x,4)>10U)return fail("prediction order above default 10");
    size_t nav=xo+common+sub; x.bit=nav*8U; unsigned segs=1U<<grid,maxseg=0;
    for(unsigned i=0;i<segs;i++){unsigned z=get(&x,sbits)+1U;if(z>maxseg)maxseg=z;}
    if(needbits(maxseg-1U)!=sbits)return fail("minimum XLL segment-size field");
    free(d); puts("PASS official-observed core/EXSS/XLL structure"); return 0;
}
