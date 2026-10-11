/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Verify core PCM-block and XLL sample-count signalling on a raw bitstream.
 * FFmpeg PCM comparison is a separate, independent acceptance test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static unsigned bits(const unsigned char *p,size_t bit,unsigned width)
{
    unsigned result=0;
    for(unsigned k=0;k<width;k++)result=(result<<1U)|((p[(bit+k)>>3]>>(7-((bit+k)&7)))&1U);
    return result;
}
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    int samples=atoi(argv[2]);
    int expected=atoi(argv[3]);
    if((samples!=512&&samples!=1024)||expected<1)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)){fclose(f);return 2;}
    long length=ftell(f);
    if(length<64||length>256L*1024*1024||fseek(f,0,SEEK_SET)){fclose(f);return 2;}
    unsigned char *data=malloc((size_t)length);if(!data){fclose(f);return 2;}
    if(fread(data,1,(size_t)length,f)!=(size_t)length){free(data);fclose(f);return 2;}
    fclose(f);
    int cores=0,xll=0,invalid=0;
    for(size_t i=0;i+24<(size_t)length;i++){
        if(!memcmp(data+i,"\x7f\xfe\x80\x01",4)){
            int count=(int)bits(data+i,39,7)+1;
            if(count*32!=samples){fprintf(stderr,"Invalid core blocks: %d samples\n",count*32);invalid=1;break;}
            cores++;
        }
        if(!memcmp(data+i,"\x41\xa2\x95\x47",4)){
            unsigned size=bits(data+i,36,8)+1;
            unsigned framebits=bits(data+i,44,5)+1;
            unsigned segpos=53U+framebits;
            unsigned seg=bits(data+i,segpos,4), log=bits(data+i,segpos+4,4);
            if(seg>4||seg+log!=(samples==512?9U:10U)||size<12){
                fprintf(stderr,"Invalid XLL sample grid: %u + %u\n",seg,log);invalid=1;break;
            }
            xll++;
        }
    }
    free(data);
    if(invalid||cores!=expected||xll!=expected){
        fprintf(stderr,"Core/XLL frame count mismatch: core=%d XLL=%d expected=%d\n",cores,xll,expected);
        return 1;
    }
    printf("PASS: %d genuine %d-sample DCA core and XLL frames\n",expected,samples);
    return 0;
}
