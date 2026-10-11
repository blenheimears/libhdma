/* Verify Matroska seek structure independently of the project muxer.
 * Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { uint32_t id; size_t pos, start, end; } Element;
static uint8_t *data;
static size_t length;
static int fail(const char *msg){fprintf(stderr,"Matroska structure error: %s\n",msg);return 1;}
static int next(size_t *at,size_t end,Element *e){
    if(*at>=end || *at>=length)return -1;
    size_t beginning=*at;uint8_t v=data[(*at)++];int n=1;
    while(n<=4 && !(v>>(8-n)&1))n++;
    if(n>4 || *at+(size_t)(n-1)>end)return -1;
    uint32_t id=v;while(--n)id=id<<8|data[(*at)++];
    if(*at>=end)return -1;
    v=data[(*at)++];n=1;while(n<=8 && !(v>>(8-n)&1))n++;
    if(n>8 || *at+(size_t)(n-1)>end)return -1;
    uint64_t size=v & ((1U<<(8-n))-1U);
    for(int i=1;i<n;i++)size=size<<8|data[(*at)++];
    if(size==((UINT64_C(1)<<(7*n))-1) || size>(uint64_t)(end-*at))return -1;
    e->id=id;e->pos=beginning;e->start=*at;e->end=*at+(size_t)size;
    *at=e->end;return 0;
}
static uint64_t number(Element e){
    if(e.end-e.start>8)return UINT64_MAX;
    uint64_t value=0;for(size_t i=e.start;i<e.end;i++)value=value<<8|data[i];
    return value;
}
static uint32_t id_value(Element e){return (uint32_t)number(e);}
static int parse_seek(Element head,size_t segment_start,uint64_t *info,uint64_t *tracks,uint64_t *cues){
    size_t at=head.start;int count=0;
    while(at<head.end){
        Element entry;if(next(&at,head.end,&entry) || entry.id!=0x4dbb)return -1;
        size_t child=entry.start;uint32_t ident=0;uint64_t position=UINT64_MAX;
        while(child<entry.end){
            Element field;if(next(&child,entry.end,&field))return -1;
            if(field.id==0x53ab)ident=id_value(field);
            if(field.id==0x53ac)position=number(field);
        }
        if(position==UINT64_MAX || position>=length-segment_start)return -1;
        if(ident==0x1549a966)*info=position,count++;
        if(ident==0x1654ae6b)*tracks=position,count++;
        if(ident==0x1c53bb6b)*cues=position,count++;
    }
    return count==3?0:-1;
}
int main(int argc,char **argv){
    if(argc!=4)return fail("usage: test_matroska FILE SAMPLES RATE");
    char *tail=NULL;uint64_t samples=strtoull(argv[2],&tail,10);
    if(!tail || *tail)return fail("invalid samples");
    long rate=strtol(argv[3],&tail,10);
    if(!tail || *tail || rate<=0)return fail("invalid rate");
    FILE*f=fopen(argv[1],"rb");if(!f)return fail("cannot open file");
    if(fseek(f,0,SEEK_END) || (long)(length= (size_t)ftell(f))<=0 ||
       length>10000000 || fseek(f,0,SEEK_SET)) {fclose(f);return fail("invalid test file size");}
    data=malloc(length);if(!data){fclose(f);return fail("allocation");}
    int result=1;
    if(fread(data,1,length,f)!=length)goto done;
    size_t at=0;Element ebml,segment;
    if(next(&at,length,&ebml) || ebml.id!=0x1a45dfa3 ||
       next(&at,length,&segment) || segment.id!=0x18538067 || segment.end!=length)
        {fail("finite Segment size or EBML header invalid");goto done;}
    at=segment.start;Element head;
    if(next(&at,segment.end,&head) || head.id!=0x114d9b74){fail("SeekHead not first");goto done;}
    uint64_t info_pos=UINT64_MAX,tracks_pos=UINT64_MAX,cues_pos=UINT64_MAX;
    if(parse_seek(head,segment.start,&info_pos,&tracks_pos,&cues_pos)){
        fail("SeekHead missing Info/Tracks/Cues positions");goto done;
    }
    uint64_t clusters[32],cluster_time[32];size_t cluster_count=0;
    size_t info_count=0,tracks_count=0,cues_count=0;
    while(at<segment.end){
        Element top;
        if(next(&at,segment.end,&top)){fail("invalid top-level element bounds");goto done;}
        uint64_t relative=(uint64_t)(top.pos-segment.start);
        if(top.id==0x1549a966){
            if(relative!=info_pos || ++info_count!=1){fail("Info seek position");goto done;}
            size_t p=top.start;int has_duration=0;
            while(p<top.end){
                Element x;if(next(&p,top.end,&x)){fail("Info element");goto done;}
                if(x.id==0x4489){
                    if(x.end-x.start!=8){fail("Duration width");goto done;}
                    uint64_t raw=number(x);double duration;memcpy(&duration,&raw,8);
                    double expected=(double)samples*1000.0/(double)rate;
                    if(!isfinite(duration) || fabs(duration-expected)>0.00001){
                        fail("Duration does not match exact sample count");goto done;
                    }
                    has_duration=1;
                }
            }
            if(!has_duration){fail("Duration missing");goto done;}
        } else if(top.id==0x1654ae6b){
            if(relative!=tracks_pos || ++tracks_count!=1){fail("Tracks seek position");goto done;}
        } else if(top.id==0x1f43b675){
            if(cluster_count>=32){fail("too many clusters in fixture");goto done;}
            clusters[cluster_count]=relative;
            size_t p=top.start;Element tc;
            if(next(&p,top.end,&tc) || tc.id!=0xe7){fail("cluster timestamp missing");goto done;}
            cluster_time[cluster_count++]=number(tc);
        } else if(top.id==0x1c53bb6b){
            if(relative!=cues_pos || ++cues_count!=1){fail("Cues seek position");goto done;}
            size_t p=top.start;size_t entries=0;
            while(p<top.end){
                Element cue;if(next(&p,top.end,&cue) || cue.id!=0xbb){fail("CuePoint structure");goto done;}
                uint64_t cue_time=UINT64_MAX,cluster_pos=UINT64_MAX,track=UINT64_MAX;
                size_t inside=cue.start;
                while(inside<cue.end){
                    Element child;if(next(&inside,cue.end,&child)){fail("CuePoint child");goto done;}
                    if(child.id==0xb3)cue_time=number(child);
                    if(child.id==0xb7){
                        size_t tp=child.start;
                        while(tp<child.end){
                            Element field;if(next(&tp,child.end,&field)){fail("CueTrackPositions child");goto done;}
                            if(field.id==0xf7)track=number(field);
                            if(field.id==0xf1)cluster_pos=number(field);
                        }
                    }
                }
                if(entries>=cluster_count || track!=1 || cue_time!=cluster_time[entries] ||
                   cluster_pos!=clusters[entries]){fail("Cue does not reference corresponding cluster");goto done;}
                entries++;
            }
            if(entries!=cluster_count){fail("not all clusters have a Cue");goto done;}
        } else {fail("unexpected top-level element");goto done;}
    }
    if(info_count!=1 || tracks_count!=1 || cues_count!=1 || cluster_count<3)
        {fail("missing top-level elements or sparse cues");goto done;}
    printf("PASS Matroska: finite Segment/Clusters, duration, SeekHead, %zu valid Cues\n",cluster_count);
    result=0;
done:
    fclose(f);free(data);return result;
}
