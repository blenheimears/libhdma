/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later. */
#define _POSIX_C_SOURCE 200809L
#include "raw_pcm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

struct RawPcmInput {
    FILE *stream;
    int owns_stream,rate,channels,depth,layout21;
    int64_t remaining;
    char error[160];
};
static int layout_channels(const char *name){
    if(!name)return 0;
    if(!strcmp(name,"mono") || !strcmp(name,"1.0"))return 1;
    if(!strcmp(name,"stereo") || !strcmp(name,"2.0"))return 2;
    if(!strcmp(name,"3.0") || !strcmp(name,"2.1"))return 3;
    if(!strcmp(name,"2.2"))return 4;
    if(!strcmp(name,"5.0"))return 5;
    if(!strcmp(name,"5.1"))return 6;
    if(!strcmp(name,"7.0"))return 7;
    if(!strcmp(name,"7.1"))return 8;
    return -1;
}
int raw_pcm_open(RawPcmInput **out,const char *path,int rate,int channels,int depth,const char *layout){
    if(!out || !path)return -1;
    *out=NULL;
    int n=layout_channels(layout);
    if(rate!=32000 && rate!=44100 && rate!=48000 && rate!=88200 && rate!=96000 && rate!=176400 && rate!=192000){
        fprintf(stderr,"raw PCM sample rate must be an explicitly specified supported rate\n");return -1;
    }
    if(channels<1 || channels>8 || (depth!=16 && depth!=24) || n<0 || (n && n!=channels) || (channels>2 && !n)){
        fprintf(stderr,"raw PCM requires explicit valid channels, 16/24-bit depth, and a matching multichannel layout\n");return -1;
    }
    RawPcmInput *s=calloc(1,sizeof(*s));if(!s)return -1;
    s->rate=rate;s->channels=channels;s->depth=depth;s->layout21=(layout && !strcmp(layout,"2.1"));s->remaining=-1;
    s->owns_stream=strcmp(path,"-")!=0;
    s->stream=s->owns_stream?fopen(path,"rb"):stdin;
    if(!s->stream){perror("open raw PCM");free(s);return -1;}
    if(s->owns_stream){
        struct stat st;
        if(fstat(fileno(s->stream),&st)==0 && S_ISREG(st.st_mode)){
            int64_t stride=(int64_t)channels*(depth/8);
            if(st.st_size<0 || st.st_size%stride){
                fprintf(stderr,"raw PCM size is not divisible by its sample frame size\n");raw_pcm_close(s);return -1;
            }
            s->remaining=st.st_size/stride;
        }
    }
    *out=s;return 0;
}
int raw_pcm_rate(const RawPcmInput *s){return s?s->rate:0;}
int raw_pcm_channels(const RawPcmInput *s){return s?s->channels:0;}
int raw_pcm_depth(const RawPcmInput *s){return s?s->depth:0;}
int raw_pcm_layout21(const RawPcmInput *s){return s?s->layout21:0;}
int64_t raw_pcm_frames(const RawPcmInput *s){return s?s->remaining:-1;}
int raw_pcm_read(RawPcmInput *s,int32_t *dest,int frames){
    if(!s || !dest || frames<1 || frames>1024)return -1;
    if(s->remaining==0)return 0;
    const size_t stride=(size_t)s->channels*(size_t)(s->depth/8);
    size_t wanted=(size_t)frames;
    if(s->remaining>0 && (int64_t)wanted>s->remaining)wanted=(size_t)s->remaining;
    unsigned char bytes[1024*8*3];
    const size_t total=wanted*stride;
    size_t read_bytes=0;
    while(read_bytes<total){
        size_t got=fread(bytes+read_bytes,1,total-read_bytes,s->stream);
        if(got){read_bytes+=got;continue;}
        if(ferror(s->stream)){snprintf(s->error,sizeof(s->error),"raw PCM input read failed");return -1;}
        break;
    }
    if(read_bytes%stride){snprintf(s->error,sizeof(s->error),"truncated raw PCM sample frame");return -1;}
    if(s->remaining>=0 && read_bytes<total){snprintf(s->error,sizeof(s->error),"raw PCM input ended before expected length");return -1;}
    size_t samples=read_bytes/(size_t)(s->depth/8);
    for(size_t i=0;i<samples;i++){
        const unsigned char *p=bytes+i*(size_t)(s->depth/8);
        if(s->depth==16){
            uint32_t u=(uint32_t)p[0]|(uint32_t)p[1]<<8;
            int32_t v=u>=0x8000u?(int32_t)u-65536:(int32_t)u;
            dest[i]=v*65536; /* libsndfile's left-justified sf_readf_int convention */
        }else{
            uint32_t u=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;
            int32_t v=u>=0x800000u?(int32_t)u-16777216:(int32_t)u;
            dest[i]=v*256;
        }
    }
    int got_frames=(int)(read_bytes/stride);
    if(s->remaining>=0)s->remaining-=got_frames;
    return got_frames;
}
const char *raw_pcm_error(const RawPcmInput *s){return s?s->error:"invalid raw input";}
void raw_pcm_close(RawPcmInput *s){if(!s)return;if(s->owns_stream && s->stream)fclose(s->stream);free(s);}
