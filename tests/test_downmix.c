/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#include "downmix.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

static int32_t legacy_blend(int32_t a,int32_t b) {
    return (int32_t)(((int64_t)a*3+b)/4);
}
static int32_t q15(int32_t sample,int32_t coeff) {
    return (int32_t)(((int64_t)sample*coeff+16384)>>15);
}
static int32_t clip32(int64_t v) {
    return v>INT32_MAX?INT32_MAX:v<INT32_MIN?INT32_MIN:(int32_t)v;
}
static int32_t q15_fold(int32_t rear,int32_t side) {
    int64_t primary=(int64_t)side+q15(rear,32767);
    return clip32((primary*23170+16384)>>15);
}

static int policies(int channels)
{
    int32_t source[512*8]={0}, saved[512*8], core[512*6]={0};
    const int lfe=channels==8, out=channels-2;
    const int back=lfe?4:3, side=lfe?6:5;
    for (int n=0;n<512;n++)for(int c=0;c<channels;c++)
        source[n*channels+c]=(n&1 ? -1 : 1)*(INT32_MAX/4-c*127);
    memcpy(saved,source,sizeof(source));
    for(int policy=HDMA_SIDE_BOTH;policy<=HDMA_SIDE_MUTE;policy++){
        if(hdma_downmix_core_policy(source,channels,core,(HdmaSidePolicy)policy)!=out ||
           memcmp(source,saved,sizeof(source)))return -1;
        for(int n=0;n<512;n++){
            int i=n*channels,o=n*out;
            int32_t fl=source[i],fr=source[i+1],fc=source[i+2];
            int32_t lfev=lfe?source[i+3]:0;
            int32_t sl=source[i+back],sr=source[i+back+1];
            if(policy==HDMA_SIDE_BOTH){
                fl=q15(source[i],23170);
                fr=q15(source[i+1],23170);
                fc=q15(source[i+2],23170);
                if(lfe)lfev=q15(source[i+3],23170);
                sl=q15_fold(source[i+back],source[i+side]);
                sr=q15_fold(source[i+back+1],source[i+side+1]);
            } else if(policy==HDMA_SIDE_FRONT){
                fl=legacy_blend(source[i],source[i+side]);
                fr=legacy_blend(source[i+1],source[i+side+1]);
            } else if(policy==HDMA_SIDE_REAR){
                sl=legacy_blend(source[i+back],source[i+side]);
                sr=legacy_blend(source[i+back+1],source[i+side+1]);
            }
            if(core[o]!=fl || core[o+1]!=fr || core[o+out-2]!=sl || core[o+out-1]!=sr ||
               core[o+2]!=fc || (lfe && core[o+3]!=lfev))return -1;
        }
    }

    /* Side-only samples must never reach fronts in the default standard fold. */
    memset(source,0,sizeof(source));
    for(int n=0;n<512;n++) {
        source[n*channels+side]=16000;
        source[n*channels+side+1]=-16000;
    }
    if(hdma_downmix_core_policy(source,channels,core,HDMA_SIDE_BOTH)!=out ||
       core[0]!=0 || core[1]!=0 ||
       core[out-2]!=q15_fold(0,16000) || core[out-1]!=q15_fold(0,-16000) ||
       core[2]!=0 || (lfe && core[3]!=0))return -1;

    /* Explicit legacy policy behavior remains available for diagnostics. */
    if(hdma_downmix_core_policy(source,channels,core,HDMA_SIDE_FRONT)!=out ||
       core[0]!=4000 || core[1]!=-4000 || core[out-2]!=0 || core[out-1]!=0)return -1;
    if(hdma_downmix_core_policy(source,channels,core,HDMA_SIDE_REAR)!=out ||
       core[0]!=0 || core[1]!=0 || core[out-2]!=4000 || core[out-1]!=-4000)return -1;
    if(hdma_downmix_core_policy(source,channels,core,HDMA_SIDE_MUTE)!=out ||
       core[0]!=0 || core[1]!=0 || core[out-2]!=0 || core[out-1]!=0)return -1;
    return 0;
}

static int exercise(int channels) {
    int32_t input[512*8] = {0}, original[512*8], core[512*6] = {0};
    int lfe = channels==8, oc=channels-2;
    int32_t pattern[8]={ 120000, -120000, 35000, 10000, 60000, -60000, 20000, -20000 };
    for(int n=0;n<512;n++)
        for(int i=0;i<channels;i++) input[n*channels+i]=pattern[i];
    memcpy(original,input,sizeof(input));
    int result=hdma_downmix_core(input,channels,core);
    if(result!=oc || memcmp(input,original,sizeof(input))) return -1;
    for(int n=0;n<512;n++) {
        int i=n*channels,o=n*oc, b=lfe?4:3,s=lfe?6:5;
        if(core[o]!=q15(input[i],23170) || core[o+1]!=q15(input[i+1],23170) ||
           core[o+2]!=q15(input[i+2],23170) ||
           (lfe && core[o+3]!=q15(input[i+3],23170)) ||
           core[o+oc-2]!=q15_fold(input[i+b],input[i+s]) ||
           core[o+oc-1]!=q15_fold(input[i+b+1],input[i+s+1])) return -1;
    }

    /* Full-scale same-polarity side+rear must stay in range and not affect fronts. */
    memset(input,0,sizeof(input));
    for(int n=0;n<512;n++) {
        input[n*channels+(lfe?4:3)]=INT32_MAX;
        input[n*channels+(lfe?6:5)]=INT32_MAX;
        input[n*channels+(lfe?5:4)]=INT32_MIN;
        input[n*channels+(lfe?7:6)]=INT32_MIN;
    }
    if(hdma_downmix_core(input,channels,core)!=oc || core[0]!=0 || core[1]!=0) return -1;
    if(core[oc-2]!=q15_fold(INT32_MAX,INT32_MAX) ||
       core[oc-1]!=q15_fold(INT32_MIN,INT32_MIN)) return -1;
    return 0;
}
int main(void) {
    int32_t dummy[512*8]={0};
    if(exercise(7) || exercise(8) || policies(7) || policies(8) ||
       hdma_downmix_core(dummy,6,dummy)!=-1 ||
       hdma_downmix_core(NULL,8,dummy)!=-1 ||
       hdma_downmix_core(dummy,8,NULL)!=-1 ||
       hdma_downmix_core_policy(dummy,8,dummy,(HdmaSidePolicy)-1)!=-1 ||
       hdma_downmix_core_policy(dummy,8,dummy,(HdmaSidePolicy)4)!=-1) {
        fputs("7.x core downmix invariant failed\n",stderr);return 1;
    }
    puts("PASS 7.0/7.1 reference -3 dB primary scale, side+rear surround fold, legacy overrides, bounds");
    return 0;
}
