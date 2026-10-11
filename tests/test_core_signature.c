/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "core_signature.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *msg) {fprintf(stderr,"core signature: %s\n",msg);return 1;}
int main(void)
{
    int32_t a[512*6],b[512*6];
    if(hdma_core_signature(a,512,6,48000,24,0,0))return fail("5.1 generation failed");
    if(hdma_core_signature(b,512,6,48000,24,0,300))return fail("cycle generation failed");
    /* The complete motif is 16 x 0.2 s; at 48 kHz it is exactly 300 frames. */
    if(memcmp(a,b,sizeof(a)))return fail("motif is not deterministic and periodic");
    int nonzero=0;
    for(int i=0;i<512;i++){
        if(a[6*i+3])return fail("LFE must be silent");
        if(a[6*i]>75000000 || a[6*i]<-75000000)return fail("level exceeded -29 dBFS");
        for(int c=1;c<6;c++)if(c!=3 && a[6*i+c]!=a[6*i])return fail("channel mismatch");
        nonzero|=a[6*i]!=0;
    }
    if(!nonzero)return fail("signature is silent");
    if(hdma_core_signature(b,512,6,48000,24,0,60))return fail("rest generation failed");
    for(unsigned i=0;i<512*6;i++)if(b[i])return fail("note rest is not silent");
    if(hdma_core_signature(b,512,3,48000,16,1,0))return fail("2.1 generation failed");
    for(int i=0;i<512;i++)if(b[3*i+2])return fail("2.1 LFE not silent");
    if(hdma_core_signature(b,512,6,48000,24,1,0)==0)return fail("invalid layout accepted");
    puts("PASS quiet repeating multi-note signature, silent LFE, periodicity, rests and parameter checks");
    return 0;
}
