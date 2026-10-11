/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Independently inspect the segment-grid field in produced XLL frames.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned field(const unsigned char *p, size_t byte_offset,
                      unsigned bit_offset, unsigned width)
{
    unsigned value = 0;
    for (unsigned i=0;i<width;i++) {
        size_t bit = byte_offset*8 + bit_offset + i;
        value = (value << 1) | ((p[bit / 8] >> (7 - (bit & 7))) & 1U);
    }
    return value;
}
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,"usage: test_xll_structure FILE 0|1|2|3|4|mixed|varied\n");
        return 2;
    }
    int expected=-1;
    int want_mixed=!strcmp(argv[2],"mixed");
    int want_varied=!strcmp(argv[2],"varied");
    if (!want_mixed && !want_varied) {
        if (argv[2][0]<'0' || argv[2][0]>'4' || argv[2][1])return 2;
        expected=argv[2][0]-'0';
    }
    FILE *fp=fopen(argv[1],"rb");
    if (!fp) { perror(argv[1]); return 1; }
    if (fseek(fp,0,SEEK_END)) { fclose(fp); return 1; }
    long length=ftell(fp);
    if (length<0 || length>64*1024*1024 || fseek(fp,0,SEEK_SET)) {
        fclose(fp);return 1;
    }
    unsigned char *buf=malloc((size_t)length+1);
    if (!buf) { fclose(fp);return 1; }
    int result=1;
    if (fread(buf,1,(size_t)length,fp)!=(size_t)length) goto done;
    unsigned count[5]={0,0,0,0,0};
    unsigned mixed=0,frames=0;
    for (size_t i=0;i+14<(size_t)length;i++) {
        if (memcmp(buf+i,"\x41\xa2\x95\x47",4)) continue;
        unsigned common=field(buf,i,36,8)+1;
        unsigned framebits=field(buf,i,44,5)+1;
        unsigned segpos=53U+framebits;
        unsigned log2segments=field(buf,i,segpos,4);
        unsigned log2samples=field(buf,i,segpos+4,4);
        unsigned navi_bits=field(buf,i,segpos+8,5)+1;
        if ((log2samples+log2segments!=9 && log2samples+log2segments!=10) ||
            navi_bits<1 || navi_bits>20 ||
            i+common+8>=(size_t)length) {
            fprintf(stderr,"Malformed XLL grid/header at %zu\n",i);
            goto done;
        }
        unsigned channels=field(buf,i+common,10,4)+1;
        if (channels>6)goto done;
        unsigned mask=field(buf,i+common,14,channels);
        if (mask && mask != (1U<<channels)-1)mixed++;
        if (log2segments>4) {
            fprintf(stderr,"Invalid XLL segment grid: %u\n",log2segments);
            goto done;
        }
        count[log2segments]++;frames++;
        i+=3;
    }
    unsigned distinct=0;
    for (unsigned g=0;g<5;g++)if(count[g])distinct++;
    if (!frames || (expected>=0 && !count[expected]) ||
        (want_mixed && !mixed) || (want_varied && distinct<2)) {
        fprintf(stderr,"No XLL frame uses %d segments (1:%u, 2:%u, 4:%u, 8:%u, 16:%u)\n",
                expected>=0 ? 1<<expected : 0,count[0],count[1],count[2],count[3],count[4]);
        goto done;
    }
    printf("PASS XLL segment grids: 1=%u, 2=%u, 4=%u, 8=%u, 16=%u; mixed-reference frames=%u\n",
           count[0],count[1],count[2],count[3],count[4],mixed);
    result=0;
done:
    free(buf);
    fclose(fp);
    return result;
}
