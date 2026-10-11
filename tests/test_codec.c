/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "libhdma.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int test_case(int channels,int rate,int bitrate) {
    DcaCore *codec=NULL;
    if(dca_core_create(&codec,channels,rate,bitrate))return 1;
    int size=dca_core_frame_bytes(codec);
    int32_t samples[512*6];uint8_t packet[16384];
    for(int frame=0;frame<3;frame++){
        for(int i=0;i<512*channels;i++)
            samples[i]=(int32_t)((((uint32_t)i*1733u+(uint32_t)frame*17u)&65535u)-32768)*1024;
        memset(packet,0x33,sizeof(packet));
        int result=dca_core_encode(codec,samples,packet,sizeof(packet));
        if(result!=size || memcmp(packet,"\x7f\xfe\x80\x01",4)!=0){
            fprintf(stderr,"core failure channels=%d rate=%d bitrate=%d\n",channels,rate,bitrate);
            dca_core_destroy(codec);return 1;
        }
    }
    dca_core_destroy(codec);return 0;
}
int main(void) {
    if(test_case(1,48000,1411200) || test_case(2,44100,1411200) ||
       test_case(3,48000,1411200) ||
       test_case(4,48000,1536000) || test_case(5,48000,1536000) ||
       test_case(6,48000,1536000))return 1;
    DcaCore*unsupported=NULL;
    if(!dca_core_create(&unsupported,7,48000,1411200))return 2;
    if(!dca_core_create(&unsupported,2,96000,1411200))return 3;
    puts("Standalone C core unit tests passed");return 0;
}
