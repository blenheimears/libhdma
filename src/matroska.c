/* Standalone Matroska muxer with exact duration, finite element sizes, SeekHead
 * and per-500ms audio Cues. No FFmpeg dependency.
 * Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#define _POSIX_C_SOURCE 200809L
#include "matroska.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>

typedef struct { uint8_t bytes[1024]; size_t length; int error; } Buffer;

static void bytes(Buffer *b, const uint8_t *p, size_t n) {
    if (b->error || n > sizeof(b->bytes)-b->length) { b->error=1;return; }
    memcpy(b->bytes+b->length,p,n);b->length+=n;
}
static void id(Buffer *b,uint32_t x) {
    unsigned n=x<=255?1:x<=65535?2:x<=16777215?3:4;
    uint8_t tmp[4];for(unsigned i=0;i<n;i++)tmp[i]=(uint8_t)(x>>(8*(n-i-1)));
    bytes(b,tmp,n);
}
static void size_vint(Buffer *b,uint64_t x) {
    for(unsigned n=1;n<=8;n++){
        if(x >= ((UINT64_C(1)<<(n*7))-1))continue;
        uint8_t tmp[8];for(unsigned i=0;i<n;i++)tmp[n-1-i]=(uint8_t)(x>>(8*i));
        tmp[0]|=(uint8_t)(1u<<(8-n));bytes(b,tmp,n);return;
    }
    b->error=1;
}
static void element(Buffer *b,uint32_t ident,const uint8_t *p,size_t n) {
    id(b,ident);size_vint(b,n);bytes(b,p,n);
}
static void uint_element_fixed(Buffer*b,uint32_t ident,uint64_t x,unsigned n){
    uint8_t p[8];if(!n || n>8){b->error=1;return;}
    for(unsigned i=0;i<n;i++)p[n-1-i]=(uint8_t)(x>>(8*i));
    element(b,ident,p,n);
}
static void uint_element(Buffer*b,uint32_t ident,uint64_t x){
    unsigned n=1;while(n<8 && (x>>(n*8)))n++;
    uint_element_fixed(b,ident,x,n);
}
static void text_element(Buffer*b,uint32_t ident,const char *s){element(b,ident,(const uint8_t*)s,strlen(s));}
static void double_element(Buffer*b,uint32_t ident,double value){
    uint64_t raw;uint8_t p[8];memcpy(&raw,&value,8);
    for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(raw>>(8*i));
    element(b,ident,p,8);
}
static void master(Buffer*b,uint32_t ident,const Buffer*n){
    if(n->error)b->error=1;else element(b,ident,n->bytes,n->length);
}
static int write_bytes(FILE*f,const uint8_t*p,size_t n){return fwrite(p,1,n,f)==n?0:-1;}
static int64_t tell(FILE*f){off_t p=ftello(f);return p<0?-1:(int64_t)p;}
static int seek(FILE*f,int64_t position){return position<0 || fseeko(f,(off_t)position,SEEK_SET)?-1:0;}
/* Write a fixed 8-byte EBML VINT, reserving the all-ones value for 'unknown'. */
static int write_size8(FILE*f,uint64_t count){
    if(count>=((UINT64_C(1)<<56)-1))return -1;
    uint64_t x=(UINT64_C(1)<<56)|count;uint8_t p[8];
    for(unsigned i=0;i<8;i++)p[7-i]=(uint8_t)(x>>(i*8));
    return write_bytes(f,p,8);
}
static int file_element(FILE*f,uint32_t ident,const uint8_t*p,size_t n){
    Buffer b={0};id(&b,ident);size_vint(&b,n);
    return b.error || write_bytes(f,b.bytes,b.length) || write_bytes(f,p,n)?-1:0;
}
static int64_t position_rel(MkaWriter*m){
    int64_t p=tell(m->file);
    return p<0 || p<m->segment_payload_pos?-1:p-m->segment_payload_pos;
}
static int patch_size(MkaWriter*m,int64_t size_pos,int64_t payload_start,int64_t end){
    if(size_pos<0 || payload_start<0 || end<payload_start ||
       seek(m->file,size_pos) || write_size8(m->file,(uint64_t)(end-payload_start)) ||
       seek(m->file,end))return -1;
    return 0;
}
static int seek_entry(Buffer*dst,uint32_t ident,uint64_t pos){
    Buffer b={0};uint8_t p[4];
    for(int i=0;i<4;i++)p[i]=(uint8_t)(ident>>(8*(3-i)));
    element(&b,0x53ab,p,4);uint_element_fixed(&b,0x53ac,pos,8);
    master(dst,0x4dbb,&b);return dst->error?-1:0;
}
static int make_seekhead(Buffer*result,uint64_t info,uint64_t tracks,uint64_t cues){
    Buffer body={0};
    if(seek_entry(&body,0x1549a966,info) || seek_entry(&body,0x1654ae6b,tracks) ||
       seek_entry(&body,0x1c53bb6b,cues))return -1;
    master(result,0x114d9b74,&body);
    return result->error?-1:0;
}
static int close_cluster(MkaWriter*m){
    if(!m->started)return 0;
    int64_t end=tell(m->file);
    return end<0 || patch_size(m,m->cluster_size_pos,m->cluster_payload_pos,end)?-1:0;
}
static int new_cluster(MkaWriter*m,uint64_t at_ms){
    if(close_cluster(m))return -1;
    if(m->cue_count==m->cue_capacity){
        size_t newcap=m->cue_capacity?m->cue_capacity*2:64;
        if(newcap<m->cue_capacity || newcap>SIZE_MAX/sizeof(*m->cues))return -1;
        MkaCue*p=realloc(m->cues,newcap*sizeof(*p));if(!p)return -1;
        m->cues=p;m->cue_capacity=newcap;
    }
    int64_t start=position_rel(m);
    if(start<0)return -1;
    m->cues[m->cue_count++]=(MkaCue){at_ms,(uint64_t)start};
    Buffer b={0};id(&b,0x1f43b675);
    if(b.error || write_bytes(m->file,b.bytes,b.length))return -1;
    m->cluster_size_pos=tell(m->file);
    if(m->cluster_size_pos<0 || write_size8(m->file,0))return -1;
    m->cluster_payload_pos=tell(m->file);
    b=(Buffer){0};uint_element(&b,0xe7,at_ms);
    if(b.error || write_bytes(m->file,b.bytes,b.length))return -1;
    m->cluster_ms=at_ms;m->started=1;return 0;
}
int mka_start(MkaWriter*m,FILE*file,int rate,int channels){
    if(!m || !file || rate<=0 || channels<1 || channels>8)return -1;
    memset(m,0,sizeof(*m));m->file=file;m->sample_rate=rate;m->channels=channels;
    Buffer ebml={0},info={0},audio={0},entry={0},tracks={0},top={0},head={0};
    uint_element(&ebml,0x4286,1);uint_element(&ebml,0x42f7,1);
    uint_element(&ebml,0x42f2,4);uint_element(&ebml,0x42f3,8);
    text_element(&ebml,0x4282,"matroska");uint_element(&ebml,0x4287,4);uint_element(&ebml,0x4285,2);
    master(&top,0x1a45dfa3,&ebml);
    if(top.error || write_bytes(file,top.bytes,top.length))return -1;
    top=(Buffer){0};id(&top,0x18538067);
    if(top.error || write_bytes(file,top.bytes,top.length))return -1;
    m->segment_size_pos=tell(file);
    if(m->segment_size_pos<0 || write_size8(file,0))return -1;
    m->segment_payload_pos=tell(file);
    if(m->segment_payload_pos<0 || make_seekhead(&head,0,0,0))return -1;
    m->seekhead_pos=tell(file);
    if(m->seekhead_pos<0 || write_bytes(file,head.bytes,head.length))return -1;
    m->info_pos=(uint64_t)position_rel(m);
    uint_element(&info,0x2ad7b1,1000000);
    text_element(&info,0x4d80,"libhdma hdmaenc");
    text_element(&info,0x5741,"hdmaenc 0.0.15 C core-residual XLL/core");
    size_t offset=info.length + 3; /* Duration ID 0x4489: 2 bytes, size: 1 byte */
    double_element(&info,0x4489,0.0);
    if(info.error || file_element(file,0x1549a966,info.bytes,info.length))return -1;
    int64_t info_end=tell(file);
    m->duration_pos=info_end<0?-1:info_end-(int64_t)info.length+(int64_t)offset;
    m->tracks_pos=(uint64_t)position_rel(m);
    uint_element(&entry,0xd7,1);uint_element(&entry,0x73c5,1);
    uint_element(&entry,0x83,2);uint_element(&entry,0x9c,0);
    text_element(&entry,0x86,"A_DTS");
    double_element(&audio,0xb5,(double)rate);uint_element(&audio,0x9f,(unsigned)channels);
    master(&entry,0xe1,&audio);master(&tracks,0xae,&entry);
    if(m->duration_pos<0 || tracks.error || file_element(file,0x1654ae6b,tracks.bytes,tracks.length))return -1;
    return 0;
}
static int prepare_block(MkaWriter*m,uint64_t position,uint8_t header[4]){
    if(!m || !m->file || position>UINT64_MAX/1000)return -1;
    uint64_t pts=(position*1000+(uint64_t)m->sample_rate/2)/(uint64_t)m->sample_rate;
    /* Audio cues every ~500 ms to support responsive media-player seeking. */
    if(!m->started || pts<m->cluster_ms || pts-m->cluster_ms>=500)
        if(new_cluster(m,pts))return -1;
    uint64_t delta=pts-m->cluster_ms;
    if(delta>INT16_MAX)return -1;
    header[0]=0x81;header[1]=(uint8_t)(delta>>8);
    header[2]=(uint8_t)delta;header[3]=0x80;
    return 0;
}
int mka_packet(MkaWriter*m,const uint8_t*packet,size_t size,uint64_t position){
    if(!packet || size<13 || size>65536)return -1;
    uint8_t header[4];if(prepare_block(m,position,header))return -1;
    Buffer b={0};id(&b,0xa3);size_vint(&b,size+4);
    if(b.error || write_bytes(m->file,b.bytes,b.length) ||
       write_bytes(m->file,header,4) || write_bytes(m->file,packet,size))return -1;
    return 0;
}
int mka_packet_trim(MkaWriter*m,const uint8_t*packet,size_t size,
                    uint64_t position,int discard){
    if(discard==0)return mka_packet(m,packet,size,position);
    if(!m || !packet || size<13 || size>65536 || discard<0 || discard>=1024)return -1;
    uint8_t header[4];if(prepare_block(m,position,header))return -1;
    uint64_t ns=((uint64_t)discard*1000000000ULL+(uint64_t)m->sample_rate/2)/(uint64_t)m->sample_rate;
    uint8_t signed_bytes[8];for(int i=0;i<8;i++)signed_bytes[7-i]=(uint8_t)(ns>>(i*8));
    Buffer block={0},padding={0},group={0};
    id(&block,0xa1);size_vint(&block,size+4);
    element(&padding,0x75a2,signed_bytes,8);
    id(&group,0xa0);size_vint(&group,block.length+4+size+padding.length);
    if(block.error || padding.error || group.error ||
       write_bytes(m->file,group.bytes,group.length) ||
       write_bytes(m->file,block.bytes,block.length) ||
       write_bytes(m->file,header,4) || write_bytes(m->file,packet,size) ||
       write_bytes(m->file,padding.bytes,padding.length))return -1;
    return 0;
}
int mka_finish(MkaWriter*m,uint64_t samples){
    if(!m || !m->file || !m->started || !m->cue_count || close_cluster(m))return -1;
    int64_t cues_rel=position_rel(m);
    if(cues_rel<0)return -1;
    Buffer b={0};id(&b,0x1c53bb6b);
    if(b.error || write_bytes(m->file,b.bytes,b.length))return -1;
    int64_t cues_size_pos=tell(m->file);
    if(cues_size_pos<0 || write_size8(m->file,0))return -1;
    int64_t cues_payload_pos=tell(m->file);
    if(cues_payload_pos<0)return -1;
    for(size_t i=0;i<m->cue_count;i++){
        Buffer pos={0},point={0},cue={0};
        uint_element(&pos,0xf7,1);uint_element(&pos,0xf1,m->cues[i].position);
        uint_element(&point,0xb3,m->cues[i].ms);master(&point,0xb7,&pos);
        master(&cue,0xbb,&point);
        if(cue.error || write_bytes(m->file,cue.bytes,cue.length))return -1;
    }
    int64_t end=tell(m->file);
    if(end<0 || patch_size(m,cues_size_pos,cues_payload_pos,end))return -1;
    /* 'Duration' is in Segment ticks (one tick = one millisecond). */
    double duration=(double)samples*1000.0/(double)m->sample_rate;
    if(seek(m->file,m->duration_pos))return -1;
    b=(Buffer){0};double_element(&b,0x4489,duration);
    if(b.error || b.length!=11 || write_bytes(m->file,b.bytes+3,8))return -1;
    b=(Buffer){0};
    if(make_seekhead(&b,m->info_pos,m->tracks_pos,(uint64_t)cues_rel) ||
       seek(m->file,m->seekhead_pos) || write_bytes(m->file,b.bytes,b.length) ||
       patch_size(m,m->segment_size_pos,m->segment_payload_pos,end))return -1;
    if(fflush(m->file) || ferror(m->file))return -1;
    mka_dispose(m);
    return 0;
}
void mka_dispose(MkaWriter*m){
    if(!m)return;
    free(m->cues);m->cues=NULL;m->cue_count=m->cue_capacity=0;
}
