/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Inspect independently decodable scalable MSB/LSB XLL output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static unsigned read_field(const unsigned char *b,size_t bit,unsigned width)
{
    unsigned result=0;
    for(unsigned k=0;k<width;k++)
        result=(result<<1U)|((b[(bit+k)>>3]>>(7-((bit+k)&7)))&1U);
    return result;
}
int main(int argc,char **argv)
{
    if(argc!=2 && (argc!=3 || strcmp(argv[2],"none")))return 2;
    int expect_split=argc==2;
    FILE *fp=fopen(argv[1],"rb");
    if(!fp)return 2;
    if(fseek(fp,0,SEEK_END))return 2;
    long len=ftell(fp);
    if(len<0 || len>64L*1024*1024 || fseek(fp,0,SEEK_SET))return 2;
    unsigned char *data=malloc((size_t)len+1);
    if(!data)return 2;
    if(fread(data,1,(size_t)len,fp)!=(size_t)len)return 2;
    fclose(fp);
    int frames=0,split=0;
    for(size_t i=0;i+256<(size_t)len;i++) {
        if(memcmp(data+i,"\x41\xa2\x95\x47",4))continue;
        unsigned common=read_field(data+i,36,8)+1;
        unsigned framebits=read_field(data+i,44,5)+1;
        unsigned segpos=53U+framebits;
        unsigned seg_log=read_field(data+i,segpos,4);
        unsigned sample_log=read_field(data+i,segpos+4,4);
        if(seg_log>4 || sample_log+seg_log!=9 || i+common+64>(size_t)len)continue;
        frames++;
        if(read_field(data+i,segpos+15U,1)) {
            /* Confirm the per-channel LSB length and the section-size field,
             * including its placement following all predictor parameters. */
            unsigned char *sub=data+i+common;
            size_t pos=10;
            unsigned ch=read_field(sub,pos,4)+1;pos+=4;
            if(ch<1 || ch>8)goto malformed;
            pos+=ch+5+5+4+2+2+1+1+1+1+(ch>6?9:6);
            unsigned decor=read_field(sub,pos++,1);
            if(decor) {
                unsigned nbits=ch<=2?1:ch<=4?2:3;
                pos+=ch*nbits;
                for(unsigned j=0;j<ch/2;j++) {
                    unsigned has_coeff=read_field(sub,pos++,1);
                    if(has_coeff)pos+=7;
                }
            }
            unsigned adapt[8];
            for(unsigned c=0;c<ch;c++) {adapt[c]=read_field(sub,pos,4);pos+=4;}
            for(unsigned c=0;c<ch;c++)if(!adapt[c])pos+=2;
            for(unsigned c=0;c<ch;c++)pos+=8*adapt[c];
            unsigned section=read_field(sub,pos,14);pos+=14;
            unsigned bits=0;
            for(unsigned c=0;c<ch;c++) {bits+=read_field(sub,pos,4);pos+=4;}
            for(unsigned c=0;c<ch;c++) {
                if(read_field(sub,pos,4))goto malformed; /* no lossy truncation */
                pos+=4;
            }
            if(!section || !bits || bits*(512U>>seg_log)!=section*8U)
                goto malformed;
            if((pos+7U)/8U+2U > read_field(sub,0,10)+1)goto malformed;
            split++;
        }
        i+=3;
    }
    free(data);
    if(!frames || (expect_split ? !split : split)) {
        fprintf(stderr,"No scalable XLL frames: total=%d split=%d\n",frames,split);
        return 1;
    }
    printf("PASS scalable XLL frames split=%d/%d, expected=%s\n",split,frames,expect_split?"split":"unsplit");
    return 0;
malformed:
    fprintf(stderr,"Malformed scalable XLL subheader\n");
    free(data);
    return 1;
}
