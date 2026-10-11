/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Compare scalar and runtime-dispatched SIMD kernels on fixed and random
 * adversarial 32-bit patterns (including INT32_MIN and tails).
 */
#include "simd.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static uint32_t rng=UINT32_C(0xd0e55211);
static uint32_t next_random(void)
{
    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;
}
static int test_case(int iteration)
{
    int32_t input[512],slow[512],fast[512];
    uint32_t scodes[512],vcodes[512];
    for(int i=0;i<512;i++){
        uint32_t r=next_random();
        switch(iteration%9){
        case 0:r=0;break;
        case 1:r=(uint32_t)i;break;
        case 2:r=(i&1)?UINT32_C(0x80000000):UINT32_C(0x7fffffff);break;
        case 3:r=UINT32_C(0xffffffff);break;
        case 4:r=(uint32_t)(i*i);break;
        default:break;
        }
        input[i]=(int32_t)r;
    }
    for(int order=0;order<=8;order++) {
        memcpy(slow,input,sizeof slow);memcpy(fast,input,sizeof fast);
        if(hdma_simd_configure("none"))return 1;
        hdma_simd_differences(slow,(unsigned)order);
        if(hdma_simd_configure("x86-64-v3"))return 2;
        hdma_simd_differences(fast,(unsigned)order);
        if(memcmp(slow,fast,sizeof slow)){
            fprintf(stderr,"SIMD differences mismatch: iteration=%d order=%d\n",iteration,order);
            return 3;
        }
    }
    for(int count=0;count<=512;count++) {
        if(hdma_simd_configure("none"))return 4;
        uint32_t sor=hdma_simd_zigzag(input,scodes,count);
        if(hdma_simd_configure("x86-64-v3"))return 5;
        uint32_t vor=hdma_simd_zigzag(input,vcodes,count);
        if(sor!=vor || memcmp(scodes,vcodes,(size_t)count*sizeof(uint32_t))){
            fprintf(stderr,"SIMD zigzag mismatch: iteration=%d count=%d\n",iteration,count);
            return 6;
        }
        for(unsigned k=0;k<=31;k++) {
            if(hdma_simd_configure("none"))return 7;
            uint64_t a=hdma_simd_rice_quotients(scodes,count,k);
            if(hdma_simd_configure("x86-64-v3"))return 8;
            uint64_t b=hdma_simd_rice_quotients(vcodes,count,k);
            if(a!=b){fprintf(stderr,"SIMD rice mismatch: iteration=%d count=%d k=%u\n",iteration,count,k);return 9;}
        }
    }
    return 0;
}
int main(void)
{
    if(hdma_simd_configure("unexpected")!=-1)return 10;
    if(hdma_simd_configure("none") || strcmp(hdma_simd_selected(),"none"))return 11;
    int available=hdma_simd_v3_available();
    if(hdma_simd_configure("auto"))return 12;
    if(strcmp(hdma_simd_selected(),available?"x86-64-v3":"none"))return 13;
    if(!available){
        if(hdma_simd_configure("x86-64-v3")!=-2)return 14;
        puts("PASS SIMD: unavailable v3 rejected; portable fallback works");return 0;
    }
    for(int i=0;i<12;i++){
        int result=test_case(i);
        if(result)return result;
    }
    puts("PASS SIMD: scalar and x86-64-v3 agree for difference orders, every tail and Rice k=0..31");
    return 0;
}
