/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Built only as an x86-64-v3 object; never enable -march=v3 globally.
 * Runtime selection is in simd.c and occurs before worker creation.
 */
#include <immintrin.h>
#include <stdint.h>

void hdma_v3_differences(int32_t data[512],unsigned order)
{
    for (unsigned pass=0;pass<order;pass++) {
        int n=511;
        /* Descending chunks preserve the original in-place difference
         * semantics; both overlapping source vectors load before store. */
        for (;n>=8;n-=8) {
            __m256i now=_mm256_loadu_si256((const __m256i *)(const void *)(data+n-7));
            __m256i prev=_mm256_loadu_si256((const __m256i *)(const void *)(data+n-8));
            _mm256_storeu_si256((__m256i *)(void *)(data+n-7),_mm256_sub_epi32(now,prev));
        }
        for (;n>0;--n)
            data[n]=(int32_t)((uint32_t)data[n]-(uint32_t)data[n-1]);
    }
}
uint32_t hdma_v3_zigzag(const int32_t *source,uint32_t *codes,int count)
{
    __m256i accumulated=_mm256_setzero_si256();
    int n=0;
    for (;n+8<=count;n+=8) {
        __m256i pcm=_mm256_loadu_si256((const __m256i *)(const void *)(source+n));
        __m256i code=_mm256_xor_si256(_mm256_slli_epi32(pcm,1),_mm256_srai_epi32(pcm,31));
        _mm256_storeu_si256((__m256i *)(void *)(codes+n),code);
        accumulated=_mm256_or_si256(accumulated,code);
    }
    uint32_t lanes[8];
    _mm256_storeu_si256((__m256i *)(void *)lanes,accumulated);
    uint32_t all=0;
    for (int i=0;i<8;i++)all|=lanes[i];
    for (;n<count;n++) {
        uint32_t value=((uint32_t)source[n]<<1) ^ (uint32_t)-(source[n]<0);
        codes[n]=value;
        all|=value;
    }
    return all;
}
uint64_t hdma_v3_quotients(const uint32_t *codes,int count,unsigned k)
{
    __m256i sumlo=_mm256_setzero_si256(),sumhi=_mm256_setzero_si256();
    __m128i shift=_mm_cvtsi32_si128((int)k);
    int n=0;
    for (;n+8<=count;n+=8) {
        __m256i values=_mm256_loadu_si256((const __m256i *)(const void *)(codes+n));
        __m256i quotients=_mm256_srl_epi32(values,shift);
        sumlo=_mm256_add_epi64(sumlo,_mm256_cvtepu32_epi64(_mm256_castsi256_si128(quotients)));
        sumhi=_mm256_add_epi64(sumhi,_mm256_cvtepu32_epi64(_mm256_extracti128_si256(quotients,1)));
    }
    uint64_t lanes[4];
    _mm256_storeu_si256((__m256i *)(void *)lanes,_mm256_add_epi64(sumlo,sumhi));
    uint64_t total=lanes[0]+lanes[1]+lanes[2]+lanes[3];
    for (;n<count;n++)total+=(uint64_t)(codes[n]>>k);
    return total;
}
