/* ADPCM analysis adapted from FFmpeg 9.0.2 libavcodec/dcaadpcm.c/h,
 * Copyright (C) 2017 Daniil Cherednik.
 * Standalone adaptations Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * No FFmpeg runtime or header dependency. */
#include "core_adpcm.h"
#include "core_math.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

static int32_t products[HDMA_ADPCM_VECTORS][10];
static pthread_once_t product_once=PTHREAD_ONCE_INIT;
static void prepare_products(void) {
    for(int id=0;id<HDMA_ADPCM_VECTORS;id++) {
        int k=0;
        for(int a=0;a<4;a++) for(int b=a;b<4;b++) {
            int64_t x=(int64_t)hdma_adpcm_vq[id][a]*hdma_adpcm_vq[id][b];
            if(a!=b)x*=2;
            products[id][k++]=(int32_t)x;
        }
    }
}
static int32_t clip23_adpcm(int64_t a) {
    if(a>8388607)return 8388607;
    if(a< -8388608)return -8388608;
    return (int32_t)a;
}
int32_t hdma_adpcm_predict(int id,const int32_t history[4]) {
    if(id<0 || id>=HDMA_ADPCM_VECTORS)return 0;
    int64_t sum=0;
    for(int i=0;i<4;i++)sum+=(int64_t)history[3-i]*hdma_adpcm_vq[id][i];
    return clip23_adpcm(norm__(sum,13));
}
int32_t hdma_adpcm_reconstruct(int id,int32_t residual,int32_t history[4]) {
    int32_t sample=clip23_adpcm((int64_t)residual + (id>=0 ? hdma_adpcm_predict(id,history) : 0));
    memmove(history,history+1,3*sizeof(history[0]));
    history[3]=sample;
    return sample;
}
static int64_t correlation(const int32_t *x,int len,int a,int b) {
    int64_t result=0;
    for(int i=0;i<len;i++)result+=(int64_t)x[i-a]*x[i-b];
    return result;
}
static uint64_t filter_error(const int16_t c[4],const int64_t corr[15],const int32_t prod[10]) {
    int64_t linear=0, quadratic=0;
    for(int i=0;i<4;i++)linear+=(int64_t)c[i]*corr[1+i];
    linear=norm__(linear,13);
    for(int i=0;i<10;i++)quadratic+=(int64_t)prod[i]*corr[5+i];
    quadratic=norm__(quadratic,26);
    int64_t error=corr[0]-2*linear+quadratic;
    return error<0 ? (uint64_t)(-(error+1))+1 : (uint64_t)error;
}
int hdma_adpcm_select(const int32_t *samples,int len,int32_t *diff) {
    if(!samples || !diff || (len!=16 && len!=32))return -1;
    uint32_t magnitude=0;
    for(int i=0;i<len+4;i++) {
        int32_t value=samples[i];
        magnitude|=(uint32_t)(value<0 ? -(int64_t)value : (int64_t)value);
    }
    if(!magnitude)return -1;
    int high_bit=0;
    for(uint32_t n=magnitude;n>1;n>>=1)high_bit++;
    int shift=high_bit-11;
    int32_t norm[36],original[36];
    for(int i=0;i<len+4;i++) {
        original[i]=norm__(samples[i],7);
        norm[i]=norm__(samples[i],shift);
    }
    int64_t corr[15]; int k=0;
    for(int i=0;i<=4;i++)for(int j=i;j<=4;j++)
        corr[k++]=correlation(norm+4,len,i,j);
    pthread_once(&product_once,prepare_products);
    int best=-1;uint64_t best_err=UINT64_MAX;
    for(int id=0;id<HDMA_ADPCM_VECTORS;id++) {
        uint64_t error=filter_error(hdma_adpcm_vq[id],corr,products[id]);
        if(error<best_err){best_err=error;best=id;}
    }
    if(best<0)return -1;
    uint64_t signal=0,error=0;
    for(int i=0;i<len;i++) {
        int64_t s=original[i+4];
        int64_t d=s-hdma_adpcm_predict(best,original+i);
        signal+=(uint64_t)(s*s);
        error+=(uint64_t)(d*d);
        if(d>INT32_MAX/128 || d<INT32_MIN/128)return -1;
        diff[i]=(int32_t)(d*128);
    }
    /* Do not activate ADPCM without a meaningful positive prediction gain.
     * Avoid division; FFmpeg's reference uses the same 10x energy criterion. */
    if(!error || signal/error<10)return -1;
    return best;
}
