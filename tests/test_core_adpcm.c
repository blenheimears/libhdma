/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Exercise genuine predicted-band selection, both core frame lengths,
 * reconstructed decoder history, baseline stability and option safety. */
#include "libhdma.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_case(int channels, int frame_samples)
{
    DcaCore *adpcm=NULL,*plain=NULL;
    int bitrate=channels>2?1536000:1411200;
    int (*create)(DcaCore**,int,int,int)=frame_samples==1024 ?
        dca_core_create_1024 : dca_core_create;
    int (*set)(DcaCore*,int)=frame_samples==1024 ?
        dca_core_set_adpcm_1024 : dca_core_set_adpcm;
    int (*encode)(DcaCore*,const int32_t*,uint8_t*,size_t)=frame_samples==1024 ?
        dca_core_encode_1024 : dca_core_encode;
    int (*reconstruct)(DcaCore*,int32_t*,size_t)=frame_samples==1024 ?
        dca_core_reconstruct_1024 : dca_core_reconstruct;
    int (*bands)(const DcaCore*)=frame_samples==1024 ?
        dca_core_adpcm_bands_1024 : dca_core_adpcm_bands;
    void (*destroy)(DcaCore*)=frame_samples==1024 ?
        dca_core_destroy_1024 : dca_core_destroy;
    int rc=1,predicted=0,different=0;
    int32_t pcm[1024*6],decoded[1024*6];
    uint8_t adpcm_packet[16384],plain_packet[16384];
    if(create(&adpcm,channels,48000,bitrate) || create(&plain,channels,48000,bitrate)) goto done;
    if(set(adpcm,1) || set(plain,2) != -1 || set(plain,0)) goto done;
    for(int frame=0;frame<10;frame++) {
        for(int n=0;n<frame_samples;n++)for(int ch=0;ch<channels;ch++) {
            double sample=sin(6.283185307179586*440.0*(frame*frame_samples+n)/48000.0);
            pcm[n*channels+ch]=(int32_t)(sample*14500.0)*65536;
        }
        int n1=encode(adpcm,pcm,adpcm_packet,sizeof(adpcm_packet));
        int n2=encode(plain,pcm,plain_packet,sizeof(plain_packet));
        if(n1<=0 || n1!=n2 || memcmp(adpcm_packet,"\x7f\xfe\x80\x01",4) ||
           reconstruct(adpcm,decoded,(size_t)frame_samples*channels)) goto done;
        if(set(adpcm,0)!=-1 || set(plain,1)!=-1) goto done;
        predicted+=bands(adpcm);
        different|=memcmp(adpcm_packet,plain_packet,(size_t)n1)!=0;
    }
    if(predicted<=0 || !different) {
        fprintf(stderr,"ADPCM was not emitted: %d channels, %d samples, predicted=%d diff=%d\n",
                channels,frame_samples,predicted,different);
        goto done;
    }
    printf("PASS core ADPCM: %d channels, %d samples, %d predicted bands\n",
            channels,frame_samples,predicted);
    rc=0;
done:
    destroy(adpcm);destroy(plain);return rc;
}
static void le16(FILE *f,unsigned n) {
    fputc(n&255,f);fputc((n>>8)&255,f);
}
static void le32(FILE *f,unsigned n) {
    le16(f,n&65535);le16(f,n>>16);
}
static int sine_wav(const char *path,int channels) {
    const unsigned count=1031,bytes=count*(unsigned)channels*3;
    FILE *f=fopen(path,"wb");if(!f)return 1;
    fwrite("RIFF",1,4,f);le32(f,36+bytes);fwrite("WAVEfmt ",1,8,f);
    le32(f,16);le16(f,1);le16(f,(unsigned)channels);
    le32(f,48000);le32(f,48000U*(unsigned)channels*3);
    le16(f,(unsigned)channels*3);le16(f,24);
    fwrite("data",1,4,f);le32(f,bytes);
    for(unsigned n=0;n<count;n++)for(int c=0;c<channels;c++) {
        int32_t sample=(int32_t)(1700000.0*sin(6.283185307179586*
           (220.0+55.0*c)*(double)n/48000.0));
        for(int b=0;b<3;b++)fputc(((uint32_t)sample>>(8*b))&255U,f);
    }
    int rc=ferror(f);if(fclose(f))rc=1;return rc;
}
int main(int argc,char **argv)
{
    if(argc==4 && !strcmp(argv[1],"--wav")) {
        int channels=atoi(argv[3]);
        return (channels==2 || channels==6) ? sine_wav(argv[2],channels) : 2;
    }
    if(argc!=1)return 2;
    return run_case(2,512) || run_case(6,512) ||
           run_case(2,1024) || run_case(6,1024);
}
