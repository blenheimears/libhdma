/* Verify real demuxer indexed seeking against original PCM within timestamp
 * rounding tolerance; an indexed seek must produce the expected audio.
 * Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc!=4)return 2;
    FILE*reference=fopen(argv[1],"rb"),*seeked=fopen(argv[2],"rb");
    if(!reference || !seeked)return 1;
    long sample=strtol(argv[3],NULL,10);
    uint8_t actual[4096],expected[4096];
    if(fread(actual,1,sizeof(actual),seeked)!=sizeof(actual))return 1;
    int matches=0,offset=0;
    for(int delta=-64;delta<=64;delta++){
        if(sample+delta<0 || fseek(reference,(sample+delta)*8L,SEEK_SET))continue;
        if(fread(expected,1,sizeof(expected),reference)!=sizeof(expected))continue;
        if(!memcmp(actual,expected,sizeof(actual))){matches=1;offset=delta;break;}
    }
    fclose(reference);fclose(seeked);
    if(!matches){fprintf(stderr,"indexed seeking produced incorrect decoded PCM\n");return 1;}
    printf("PASS independent FFmpeg indexed seeking: within %d frames of requested PCM\n",offset);
    return 0;
}
