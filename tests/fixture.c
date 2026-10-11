/* Deterministic PCM WAV fixture maker, no FFmpeg dependencies.
 * Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <math.h>
static void w16(FILE*f,unsigned x){fputc(x&255,f);fputc((x>>8)&255,f);}
static void w32(FILE*f,uint32_t x){for(int k=0;k<4;k++)fputc((x>>(8*k))&255,f);}
int main(int argc,char**argv){
    if(argc<6 || argc>8){fprintf(stderr,"usage fixture FILE CHANNELS RATE BITS FRAMES [WAVEX_MASK] [extremes|smooth|silence|correlated|quadratic|cubic|musical|periodic10|crosspair|lsbnoise]\n");return 2;}
    char*e;
    errno=0;long channels=strtol(argv[2],&e,10);if(errno||*e||channels<1||channels>8)return 2;
    long rate=strtol(argv[3],&e,10);if(*e||rate<8000||rate>48000)return 2;
    long bits=strtol(argv[4],&e,10);if(*e||(bits!=16 && bits!=24))return 2;
    long frames=strtol(argv[5],&e,10);if(*e||frames<1||frames>100000)return 2;
    uint32_t payload=(uint32_t)(frames*channels*(bits/8));
    int extremes=(argc>=7 && !strcmp(argv[argc-1],"extremes"));
    int smooth=(argc>=7 && !strcmp(argv[argc-1],"smooth"));
    int silent=(argc>=7 && !strcmp(argv[argc-1],"silence"));
    int correlated=(argc>=7 && !strcmp(argv[argc-1],"correlated"));
    int quadratic=(argc>=7 && !strcmp(argv[argc-1],"quadratic"));
    int cubic=(argc>=7 && !strcmp(argv[argc-1],"cubic"));
    int musical=(argc>=7 && !strcmp(argv[argc-1],"musical"));
    int periodic10=(argc>=7 && !strcmp(argv[argc-1],"periodic10"));
    int crosspair=(argc>=7 && !strcmp(argv[argc-1],"crosspair"));
    int lsbnoise=(argc>=7 && !strcmp(argv[argc-1],"lsbnoise"));
    int mode=extremes || smooth || silent || correlated || quadratic || cubic || musical || periodic10 || crosspair || lsbnoise;
    int wavex=(argc==7 && !mode) || argc==8;
    if(argc==8 && !mode)return 2;
    unsigned long mask=0;
    if(wavex){errno=0;mask=strtoul(argv[6],&e,0);if(errno||*e||mask>UINT32_MAX)return 2;}
    FILE*f=fopen(argv[1],"wb");if(!f){perror("fixture");return 1;}
    fwrite("RIFF",1,4,f);w32(f,payload+(wavex?60:36));fwrite("WAVEfmt ",1,8,f);
    w32(f,wavex?40:16);w16(f,wavex?0xfffe:1);w16(f,(unsigned)channels);w32(f,(uint32_t)rate);
    w32(f,(uint32_t)(rate*channels*(bits/8)));w16(f,(unsigned)(channels*(bits/8)));
    w16(f,(unsigned)bits);
    if(wavex){
        static const uint8_t pcm_guid[16]={1,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113};
        w16(f,22);w16(f,(unsigned)bits);w32(f,(uint32_t)mask);
        if(fwrite(pcm_guid,1,16,f)!=16){fclose(f);return 1;}
    }
    fwrite("data",1,4,f);w32(f,payload);
    for(long n=0;n<frames;n++)for(long ch=0;ch<channels;ch++){
        int32_t sample=((int32_t)((n*9973+ch*28131+n*n*13)&65535)-32768);
        if(bits==24)sample=sample*128 + (int32_t)((n*59+ch*113)&127);
        if(smooth){
            sample=(int32_t)(((n*5+ch*127)&4095)-2048);
            if(bits==24)sample=sample*256+(int32_t)((n*3+ch*11)&255);
        }
        if(correlated){
            int64_t t=n;
            int32_t base=(int32_t)(((t*t/301+t*23)%40000)-20000);
            int32_t value=base;
            if(channels==2 || channels==4 || channels==6){
                if(ch==1)value=base+(int32_t)(n%5)-2;
                else if(ch==2)value=base/2;
                else if(ch==3)value=base/8;
                else if(ch==4)value=base/3;
                else if(ch==5)value=base/3+(int32_t)(n%7)-3;
            }else if(channels==5){
                if(ch==1)value=base+(int32_t)(n%5)-2;
                else if(ch==2)value=base/2;
                else if(ch==3)value=base/3;
                else if(ch==4)value=base/3+(int32_t)(n%7)-3;
            }
            sample=bits==24?value*256+(int32_t)(n%13):value;
        }
        if(quadratic || cubic){
            int64_t t=n%512;
            int32_t value=quadratic?(int32_t)(t*t/16-7000):
                                  (int32_t)(t*t*t/10000-7000);
            value+=(int32_t)ch*11;
            sample=bits==24?value*256+(int32_t)(n%7):value;
        }
        if(silent)sample=0;
        if(extremes){
            static const int32_t boundary24[]={-8388608,8388607,-1,0,1,5592405,-5592405,4194304};
            int32_t extreme=boundary24[((unsigned long)n+(unsigned long)ch)%8];
            sample=bits==24?extreme:(extreme==-8388608?-32768:
                extreme==8388607?32767:extreme>32767?32767:extreme<-32768?-32768:extreme);
        }
        if(musical){
            /* Tonal multichannel material where a few core-reference frames
             * genuinely beat full-PCM XLL: exercise AUTO's mixed decision. */
            double t=(double)n/(double)rate;
            double a=1400000.0*sin(6.283185307179586*(103.0+23.0*ch)*t);
            double b=800000.0*sin(6.283185307179586*(401.0+17.0*ch)*t);
            sample=(int32_t)llround((a+b)/(bits==16?256.0:1.0));
        }
        if(periodic10){
            /* A tenth-order predictor can exploit this deliberately
             * irregular ten-sample cycle; lower-order fixed predictors
             * cannot reproduce it without carrying residual information. */
            static const int32_t cycle[10]={123456,-789012,345678,-234567,
                777777,-456789,654321,-135792,246813,-567891};
            sample=cycle[(unsigned)n%10];
            if(bits==16) sample/=256;
        }
        if(crosspair){
            /* Only C and R are strongly correlated in this six-channel
             * fixture (not one of 0.0.19's fixed pairs). All channels
             * remain independent speakers in the written WAV. */
            int32_t base=(int32_t)(((n*173+n*n*19)%32000)-16000);
            int32_t value=ch==0 ? base : ch==2 ? base+(int32_t)(n%3)-1 :
                (int32_t)((n*9973+ch*28131+n*n*(13+ch))&65535)-32768;
            sample=bits==24?value*128:value;
        }
        if(lsbnoise) {
            /* Independently varying lower eight bits + highly predictable
             * upper 16-bit waveform: forces genuine scalable XLL selection.
             * The linear predictor crosses zero to exercise signed splitting. */
            int32_t upper=(int32_t)(((uint32_t)n*43U+(uint32_t)ch*997U)%36000U)-18000;
            uint32_t hash=((uint32_t)n+1U)*UINT32_C(0x9e3779b1)+
                          ((uint32_t)ch+1U)*UINT32_C(0x85ebca6b);
            hash^=hash>>16;hash*=UINT32_C(0x7feb352d);hash^=hash>>15;
            sample=bits==24?upper*256+(int32_t)(hash&255U):upper;
        }
        for(int k=0;k<bits/8;k++)fputc((uint32_t)sample>>(8*k)&255,f);
    }
    int failed=fclose(f);return failed?1:0;
}
