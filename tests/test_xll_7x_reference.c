/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Inspect single-set and reference-style hierarchical 7.x XLL flags.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef struct { const unsigned char *p; size_t bits,pos; int error; } BR;
static unsigned get(BR *b,unsigned n){unsigned v=0;if(n>32||b->pos+n>b->bits){b->error=1;return 0;}while(n--)v=(v<<1)|((b->p[b->pos/8]>>(7-(b->pos&7)))&1U),b->pos++;return v;}
static int parse_set(const unsigned char *p,size_t bytes,unsigned maskbits,int hier_ofs,
                     unsigned *channels,unsigned *full,unsigned *mask)
{
    BR b={p,bytes*8,0,0}; unsigned size=get(&b,10)+1; if(size>bytes)return -1;
    *channels=get(&b,4)+1; *full=get(&b,*channels);
    get(&b,5);get(&b,5);get(&b,4);get(&b,2);get(&b,2);
    unsigned primary=get(&b,1),dmix=get(&b,1);
    if(dmix){get(&b,1);if(primary)get(&b,3);} get(&b,1);
    if(dmix&&!primary) for(int row=0;row<hier_ofs;row++){get(&b,9);for(unsigned c=0;c<*channels;c++)get(&b,9);}
    if(!get(&b,1))return -1; *mask=get(&b,maskbits);
    return b.error?-1:(int)size;
}
int main(int argc,char **argv)
{
    if((argc!=3 && argc!=4) || (strcmp(argv[2],"7") && strcmp(argv[2],"8")) ||
       (argc==4 && strcmp(argv[3],"hier") && strcmp(argv[3],"single") &&
        strcmp(argv[3],"hier-stable") && strcmp(argv[3],"hier-full")))return 2;
    int expected=atoi(argv[2]); FILE *f=fopen(argv[1],"rb");if(!f)return 2;
    fseek(f,0,SEEK_END);long length=ftell(f);fseek(f,0,SEEK_SET);
    if(length<0||length>64*1024*1024)return 2;
    unsigned char *buf=malloc((size_t)length);if(!buf)return 2;
    if(fread(buf,1,(size_t)length,f)!=(size_t)length)return 2;fclose(f);
    int single=0,hier=0,hier_ref=0;
    for(size_t i=0;i+32<(size_t)length;i++){
        if(memcmp(buf+i,"\x41\xa2\x95\x47",4))continue;
        BR b={buf+i,((size_t)length-i)*8,32,0};get(&b,4);unsigned common=get(&b,8)+1;
        unsigned fsbits=get(&b,5)+1;get(&b,fsbits);unsigned sets=get(&b,4)+1;
        get(&b,4);get(&b,4);get(&b,5);get(&b,2);get(&b,1);unsigned maskbits=get(&b,5)+1;
        if(b.error||i+common>=(size_t)length)goto fail;
        const unsigned char *sub=buf+i+common;size_t remain=(size_t)length-i-common;
        unsigned ch0,full0,mask0;int n0=parse_set(sub,remain,maskbits,0,&ch0,&full0,&mask0);if(n0<0)goto fail;
        if(sets==1){
            if(ch0!=(unsigned)expected)goto fail;
            unsigned oldmask=expected==7?0x60U:0xc0U;
            unsigned all=(1U<<expected)-1U;
            if(full0!=oldmask && full0!=all)goto fail;
            single++;
        }else if(sets==2 && expected==8){
            unsigned ch1,full1,mask1;int n1=parse_set(sub+n0,remain-(size_t)n0,maskbits,6,&ch1,&full1,&mask1);if(n1<0)goto fail;
            if(ch0!=6||full0>0x3fU||mask0!=0x627U||ch1!=2||full1!=3||mask1!=0x180U)goto fail;
            hier++; if(full0!=0x3fU)hier_ref++;
        }else goto fail;
        i+=3;
    }
    free(buf);
    if(!single && !hier)return 1;
    if(argc==4) {
        if(!strcmp(argv[3],"hier") && (!hier || !hier_ref))return 1;
        if(!strcmp(argv[3],"single") && (!single || hier))return 1;
        if(!strcmp(argv[3],"hier-stable") && (!hier || single || !hier_ref))return 1;
        if(!strcmp(argv[3],"hier-full") && (!hier || single || hier_ref))return 1;
    }
    printf("PASS %d-channel XLL: %d single-set frames, %d hierarchical frames, %d with core references\n",expected,single,hier,hier_ref);
    return 0;
fail:
    free(buf);return 1;
}
