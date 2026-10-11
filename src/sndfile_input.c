/* C-only libsndfile input. Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Uses libsndfile's stable ABI without bundling libsndfile or FFmpeg.
 */
#include "sndfile_input.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <ctype.h>

typedef int64_t SfCount;
typedef struct {
    SfCount frames;
    int samplerate, channels, format, sections, seekable;
} SfInfo;
enum { SF_READ=0x10, SF_TYPEMASK=0x0fff0000, SF_SUBMASK=0x0000ffff,
       SF_PCM16=0x0002, SF_PCM24=0x0003, SF_WAV=0x010000,
       SF_AIFF=0x020000, SF_WAVEX=0x130000, SF_FLAC=0x170000,
       /* Input channel order is validated using each format's own metadata,
        * rather than libsndfile's unsafe channel-map command. */ };
struct PcmFileInput {
    void *library, *sndfile;
    SfInfo info;
    int depth, layout21, order[8];
    SfCount remaining;
    char error[240];
    void *(*sf_open)(const char*,int,SfInfo*);
    SfCount (*sf_readf_int)(void*,int*,SfCount);
    int (*sf_close)(void*);
    const char *(*sf_strerror)(void*);
};
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static int valid_mask(uint32_t mask, int channels) {
    /* Accepted layouts have a documented canonical interleaved sample order.
     * Do not guess an unspecified surround order or drop speaker metadata. */
    switch (channels) {
    case 3: return mask == 0x7 || mask == 0xb;                    /* FL FR FC, 3.0 */
    case 4: return mask == 0x33 || mask == 0x603;  /* quad rear or side */
    case 5: return mask == 0x37 || mask == 0x607;  /* 5.0 rear or side */
    case 6: return mask == 0x3f || mask == 0x60f;  /* 5.1 rear or side */
    case 7: return mask == 0x637;                  /* 7.0 FL FR FC BL BR SL SR */
    case 8: return mask == 0x63f;                  /* 7.1 FL FR FC LFE BL BR SL SR */
    default: return 0;
    }
}
/* WAVEX speaker positions live in the fmt chunk. libsndfile doesn't promise
 * to expose this information through SFC_GET_CHANNEL_MAP_INFO for WAVEX. */
static int wavex_mask(const char *path, uint32_t *mask) {
    FILE *f=fopen(path,"rb");
    uint8_t hdr[12], chunk[8], fmt[40];
    int result=-1;
    if(!f)return -1;
    if(fread(hdr,1,12,f)!=12 || memcmp(hdr,"RIFF",4) || memcmp(hdr+8,"WAVE",4))goto done;
    for(unsigned chunks=0;chunks<128;chunks++) {
        if(fread(chunk,1,8,f)!=8)break;
        uint32_t size=le32(chunk+4);
        if(!memcmp(chunk,"fmt ",4)) {
            if(size<40 || fread(fmt,1,40,f)!=40 || fmt[0]!=0xfe || fmt[1]!=0xff ||
               fmt[16]<22 || fmt[17]!=0)break;
            *mask=le32(fmt+20);
            result=0;
            break;
        }
        /* All chunk lengths are RIFF uint32; skip safely without overflow. */
        if(fseek(f,(long)size,SEEK_CUR) || ((size&1u) && fseek(f,1,SEEK_CUR)))break;
    }
done:
    fclose(f);
    return result;
}
/* FLAC has a canonical layout for 4/5/6 channels, but a Vorbis-comment
 * WAVEFORMATEXTENSIBLE_CHANNEL_MASK may override it. Never ignore that tag. */
static int flac_mask(const char *path, uint32_t *mask, int *present) {
    FILE *f=fopen(path,"rb");
    uint8_t hdr[4];
    int result=-1;
    *present=0;
    if(!f)return -1;
    if(fread(hdr,1,4,f)!=4 || memcmp(hdr,"fLaC",4))goto done;
    for(unsigned block=0;block<128;block++) {
        if(fread(hdr,1,4,f)!=4)goto done;
        unsigned last=hdr[0]&0x80, type=hdr[0]&0x7f;
        uint32_t size=((uint32_t)hdr[1]<<16)|((uint32_t)hdr[2]<<8)|hdr[3];
        if(type==4) {
            uint8_t *buf=malloc(size ? size : 1);
            if(!buf)goto done;
            if(fread(buf,1,size,f)!=size){free(buf);goto done;}
            size_t pos=0;
            if(size<8){free(buf);goto done;}
            uint32_t vendor=le32(buf);
            if(vendor>size-8){free(buf);goto done;}
            pos=(size_t)vendor+4;
            uint32_t count=le32(buf+pos);pos+=4;
            if(count>(size-pos)/4){free(buf);goto done;}
            for(uint32_t i=0;i<count;i++) {
                if(size-pos<4){free(buf);goto done;}
                uint32_t len=le32(buf+pos);pos+=4;
                if(len>size-pos){free(buf);goto done;}
                const char *key="WAVEFORMATEXTENSIBLE_CHANNEL_MASK=";
                size_t keylen=strlen(key);
                if(len>keylen && !strncasecmp((const char *)buf+pos,key,keylen)) {
                    size_t n=len-keylen;
                    if(n<3 || n>10 || buf[pos+keylen]!='0' ||
                       (buf[pos+keylen+1]!='x' && buf[pos+keylen+1]!='X')) {
                        free(buf);goto done;
                    }
                    uint32_t value=0;
                    for(size_t k=keylen+2;k<len;k++) {
                        unsigned char c=buf[pos+k];
                        unsigned digit=(unsigned)(c>='0'&&c<='9' ? c-'0' :
                                      c>='a'&&c<='f' ? c-'a'+10 :
                                      c>='A'&&c<='F' ? c-'A'+10 : 16);
                        if(digit>15){free(buf);goto done;}
                        value=(value<<4)|digit;
                    }
                    if(*present && *mask!=value){free(buf);goto done;}
                    *mask=value;*present=1;
                }
                pos+=len;
            }
            free(buf);
        } else if(fseek(f,(long)size,SEEK_CUR))goto done;
        if(last){result=0;break;}
    }
done:
    fclose(f);
    return result;
}
static int expected_channels(const char *layout) {
    if (!layout) return 0;
    if (!strcmp(layout,"3.0") || !strcmp(layout,"2.1")) return 3;
    if (!strcmp(layout,"mono") || !strcmp(layout,"1.0")) return 1;
    if (!strcmp(layout,"stereo") || !strcmp(layout,"2.0")) return 2;
    if (!strcmp(layout,"2.2")) return 4;
    if (!strcmp(layout,"5.0")) return 5;
    if (!strcmp(layout,"5.1")) return 6;
    if (!strcmp(layout,"7.0")) return 7;
    if (!strcmp(layout,"7.1")) return 8;
    return -1;
}
int pcm_file_input_open(PcmFileInput **out,const char*filename,const char *layout_override) {
    if(!out || !filename)return -1;
    *out=NULL;
    PcmFileInput *in=calloc(1,sizeof(*in));if(!in)return -1;
    const char *lib=getenv("LIBHDMA_SNDFILE_LIBRARY");
    in->library=dlopen(lib && *lib ? lib : "libsndfile.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!in->library){fprintf(stderr,"libsndfile: %s\n",dlerror());goto fail;}
#define LOAD(symbol) do { *(void **)(&in->symbol)=dlsym(in->library,#symbol); if(!in->symbol)goto fail; }while(0)
    LOAD(sf_open); LOAD(sf_readf_int); LOAD(sf_close); LOAD(sf_strerror);
#undef LOAD
    in->sndfile=in->sf_open(filename,SF_READ,&in->info);
    if(!in->sndfile){fprintf(stderr,"libsndfile: %s\n",in->sf_strerror(NULL));goto fail;}
    int type=in->info.format & SF_TYPEMASK;
    int subtype=in->info.format & SF_SUBMASK;
    if(!((type==SF_WAV || type==SF_WAVEX || type==SF_AIFF || type==SF_FLAC) &&
         (subtype==SF_PCM16 || subtype==SF_PCM24))){
        fprintf(stderr,"input must be integer PCM16/PCM24 WAV, WAVEX, AIFF or FLAC\n");goto fail;
    }
    int channels=in->info.channels;
    if(in->info.sections!=1 || in->info.frames<=0 || channels<1 || channels>8){
        fprintf(stderr,"unsupported input sections, duration or channel count\n");goto fail;
    }
    int expected=expected_channels(layout_override);
    if(expected<0 || (expected && channels!=expected)) {
        fprintf(stderr,"--channel-layout must match the source channel count (mono, 2.0, 2.1, 3.0, 2.2, 5.0, 5.1, 7.0 or 7.1)\n");goto fail;
    }
    for(int i=0;i<channels;i++)in->order[i]=i;
    if(channels>2) {
        /* WAVEX/FLAC define the channel order via their channel mask (or the
         * FLAC canonical layout). libsndfile's SFC_GET_CHANNEL_MAP_INFO has
         * an upstream out-of-bounds read for some invalid AIFF maps, so do
         * not invoke it for any input type. */
        if(type==SF_WAVEX) {
            uint32_t mask=0;
            if(wavex_mask(filename,&mask) || (mask && !valid_mask(mask,channels))) {
                fprintf(stderr,"unsupported WAVEX speaker mask; supported: 3.0, 2.2, 5.0, 5.1, 7.0, 7.1\n");goto fail;
            }
            if (channels==3 && mask && layout_override &&
                ((mask==0xb)!=(!strcmp(layout_override,"2.1")))) {
                fprintf(stderr,"channel layout contradicts the WAVEX speaker mask\n");goto fail;
            }
            if(channels==3 && mask==0xb)in->layout21=1;
            if(!mask && !expected){
                fprintf(stderr,"WAVEX has no speaker mask; specify --channel-layout 3.0, 2.2, 5.0, 5.1, 7.0 or 7.1\n");goto fail;
            }
        } else if(type==SF_FLAC) {
            uint32_t mask=0;int present=0;
            if(flac_mask(filename,&mask,&present) || (present && mask && !valid_mask(mask,channels))) {
                fprintf(stderr,"unsupported FLAC speaker mask; supported: 3.0, 2.2, 5.0, 5.1, 7.0, 7.1\n");goto fail;
            }
            if(channels==3 && present && mask && layout_override &&
               ((mask==0xb)!=(!strcmp(layout_override,"2.1")))) {
                fprintf(stderr,"channel layout contradicts the FLAC speaker mask\n");goto fail;
            }
            if(channels==3 && present && mask==0xb)in->layout21=1;
            if(present && !mask && !expected){
                fprintf(stderr,"FLAC speaker mask is undefined; specify --channel-layout\n");goto fail;
            }
            /* Standard seven-channel FLAC uses 6.1 sample ordering (with
             * LFE and rear-center), not this encoder's supported 7.0 layout.
             * Even an explicit --channel-layout cannot safely repurpose those
             * channels into two rear speakers. Convert to correctly tagged
             * 7.0 WAVEX instead; do not silently misroute the LFE. */
            if (channels == 7) {
                fprintf(stderr,"seven-channel FLAC uses 6.1 ordering, which is not yet supported; use a correctly tagged 7.0 WAVEX source\n");goto fail;
            }
            /* Other FLAC channel arrangements follow canonical order. */
        } else if(type==SF_AIFF && !expected) {
            fprintf(stderr,"AIFF has no trusted speaker map; specify --channel-layout 3.0, 2.2, 5.0, 5.1, 7.0 or 7.1\n");goto fail;
        }
        /* 7.0/7.1 must have a trusted mask, or an explicit layout option. */
        if (type == SF_WAV && channels >= 7 && !expected) {
            fprintf(stderr,"multichannel WAV without a speaker mask needs --channel-layout 7.0 or 7.1\n");goto fail;
        }
        /* Legacy PCM WAV up to 5.1 retains the prior convention. */
    }
    if(channels==3 && layout_override && !strcmp(layout_override,"2.1"))in->layout21=1;
    in->depth=subtype==SF_PCM16?16:24;
    in->remaining=in->info.frames;
    *out=in;return 0;
fail:
    pcm_file_input_close(in);return -1;
}
int pcm_file_input_rate(const PcmFileInput*s){return s?s->info.samplerate:0;}
int pcm_file_input_channels(const PcmFileInput*s){return s?s->info.channels:0;}
int pcm_file_input_depth(const PcmFileInput*s){return s?s->depth:0;}
int pcm_file_input_layout21(const PcmFileInput*s){return s?s->layout21:0;}
int64_t pcm_file_input_frames(const PcmFileInput*s){return s?s->info.frames:0;}
int pcm_file_input_read(PcmFileInput*s,int32_t *pcm,int frames){
    if(!s || !pcm || frames<1 || frames>512)return -1;
    if(s->remaining==0)return 0;
    SfCount want=s->remaining<frames?s->remaining:frames;
    int32_t source[512*8];
    SfCount got=s->sf_readf_int(s->sndfile,(int*)source,want);
    if(got!=want){
        snprintf(s->error,sizeof(s->error),"libsndfile short read: %lld/%lld (%s)",
                 (long long)got,(long long)want,s->sf_strerror(s->sndfile));
        return -1;
    }
    int n=s->info.channels;
    for(SfCount frame=0;frame<got;frame++)
        for(int ch=0;ch<n;ch++)
            pcm[(size_t)frame*n+ch]=source[(size_t)frame*n+s->order[ch]];
    s->remaining-=got;return (int)got;
}
const char*pcm_file_input_error(const PcmFileInput*s){return s?s->error:"invalid input";}
void pcm_file_input_close(PcmFileInput*s){
    if(!s)return;
    if(s->sndfile && s->sf_close)s->sf_close(s->sndfile);
    if(s->library)dlclose(s->library);
    free(s);
}
