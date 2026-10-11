/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Portable, bit-exact reference functions and one-time backend selection.
 */
#include "simd.h"
#include <string.h>

typedef struct HdmaDsp {
    void (*differences)(int32_t data[512], unsigned order);
    uint32_t (*zigzag)(const int32_t *source,uint32_t *codes,int count);
    uint64_t (*quotients)(const uint32_t *codes,int count,unsigned k);
    const char *name;
} HdmaDsp;

static void scalar_differences(int32_t data[512], unsigned order)
{
    for (unsigned pass=0; pass<order; ++pass)
        for (int n=511; n>0; --n)
            data[n]=(int32_t)((uint32_t)data[n]-(uint32_t)data[n-1]);
}
static uint32_t scalar_zigzag(const int32_t *source,uint32_t *codes,int count)
{
    uint32_t all=0;
    for (int n=0;n<count;n++) {
        uint32_t value=((uint32_t)source[n]<<1) ^ (uint32_t)-(source[n]<0);
        codes[n]=value;
        all|=value;
    }
    return all;
}
static uint64_t scalar_quotients(const uint32_t *codes,int count,unsigned k)
{
    uint64_t total=0;
    for (int n=0;n<count;n++)total+=(uint64_t)(codes[n]>>k);
    return total;
}
static const HdmaDsp reference={scalar_differences,scalar_zigzag,scalar_quotients,"none"};
static const HdmaDsp *selected=&reference;

#if defined(HDMA_COMPILED_X86_V3)
extern void hdma_v3_differences(int32_t data[512], unsigned order);
extern uint32_t hdma_v3_zigzag(const int32_t *source,uint32_t *codes,int count);
extern uint64_t hdma_v3_quotients(const uint32_t *codes,int count,unsigned k);
static const HdmaDsp v3={hdma_v3_differences,hdma_v3_zigzag,hdma_v3_quotients,"x86-64-v3"};
#endif
int hdma_simd_v3_available(void)
{
#if defined(HDMA_COMPILED_X86_V3) && (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(_M_X64))
    __builtin_cpu_init();
    /* Validate the complete x86-64-v3 ISA, not just AVX2. In particular,
     * ensure the OS enables YMM state through its AVX capability check. */
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi") &&
           __builtin_cpu_supports("bmi2") && __builtin_cpu_supports("fma") &&
           __builtin_cpu_supports("f16c") && __builtin_cpu_supports("lzcnt") &&
           __builtin_cpu_supports("movbe") && __builtin_cpu_supports("sse3") &&
           __builtin_cpu_supports("ssse3") && __builtin_cpu_supports("sse4.1") &&
           __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("avx") &&
           __builtin_cpu_supports("xsave") && __builtin_cpu_supports("popcnt");
#else
    return 0;
#endif
}
int hdma_simd_configure(const char *request)
{
    if (!request || strcmp(request,"auto")==0) {
#if defined(HDMA_COMPILED_X86_V3)
        selected=hdma_simd_v3_available()?&v3:&reference;
#else
        selected=&reference;
#endif
        return 0;
    }
    if (strcmp(request,"none")==0) { selected=&reference;return 0; }
    if (strcmp(request,"x86-64-v3")==0) {
#if defined(HDMA_COMPILED_X86_V3)
        if (hdma_simd_v3_available()) {selected=&v3;return 0;}
#endif
        return -2; /* unavailable, never execute unsupported instructions */
    }
    return -1;
}
const char *hdma_simd_selected(void) {return selected->name;}
void hdma_simd_differences(int32_t data[512],unsigned order) {selected->differences(data,order);}
uint32_t hdma_simd_zigzag(const int32_t *src,uint32_t *codes,int count) {return selected->zigzag(src,codes,count);}
uint64_t hdma_simd_rice_quotients(const uint32_t *codes,int count,unsigned k) {return selected->quotients(codes,count,k);}
