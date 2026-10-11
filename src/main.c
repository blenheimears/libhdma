/* Standalone DCA core and DCA + XLL C CLI.
 * Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later.
 * FFmpeg is not linked or invoked by this program. FFmpeg decoder is used
 * only by optional test scripts for independent bitstream validation.
 */
#define _POSIX_C_SOURCE 200809L
#include "libhdma.h"
#include "sndfile_input.h"
#include "raw_pcm_input.h"
#include "matroska.h"
#include "xll_workers.h"
#include "xll.h"
#include "progress.h"
#include "simd.h"
#include "downmix.h"
#include "core_signature.h"
#include "cpu_count.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

static const int bitrates[] = {
    32000,56000,64000,96000,112000,128000,192000,224000,256000,320000,
    384000,448000,512000,576000,640000,768000,896000,1024000,1152000,
    1280000,1344000,1408000,1411200,1472000,1536000,1920000,2048000,
    3072000,3840000
};
static void help(FILE *stream) {
    fprintf(stream,
        "hdmaenc 0.0.49\n"
        "Usage: hdmaenc INPUT.[wav|aiff|flac] [-o OUTPUT.mka|.dca]\n"
        "       hdmaenc - --raw-pcm --sample-rate 48000 --channels 6 --bit-depth 24 --channel-layout 5.1 -o OUTPUT.dca\n"
        "               [--bitrate BITS_PER_SECOND] [--channel-layout mono|1.0|2.0|2.1|3.0|2.2|5.0|5.1|7.0|7.1] [--lossless|--lossy|--mode core]\n"
        "               [--debug-core-melody] [--prediction-order 0..15] [--max-fixed-prediction-order 0] [--xll-parameter-reuse auto|off] [--xll-decorrelation auto|on|off] [--xll-max-segments 2|4|8|16] [--xll-hardware-test] [--xll-7x-hierarchy off|on|auto] [--core-side both|front|rear|mute] [--adpcm] [--ultra] [--core-lookahead|--no-core-lookahead] [--xll-frame-samples 512|1024] [--xll-reference auto|full|core] [--threads N] [--simd auto|none|x86-64-v3] [--progress|--no-progress] [--ratio-codec|--ratio-container] [--overwrite]\n"
        "       hdmaenc --help\n"
        "Default output: INPUT.mka (lossless DCA + XLL); core nominal bitrate: 1536000 (1509 kb/s payload at 48 kHz).\n"
        "--debug-core-melody: EXPERIMENTAL, NOT FOR RELEASE DISCS. Replace only the lossy core\n"
        "                     with a repeating low-level, multi-note chime; keep original PCM\n"
        "                     in independent full-PCM XLL (bit-exact decoded HD audio).\n"
        "                     LFE is silent. Not valid with --lossy or\n"
        "                     explicitly requested --xll-reference core.\n"
        "Raw PCM input: signed interleaved little-endian 16/24-bit; supply format explicitly; - reads stdin.\n"
        "Input: integer PCM16/24; mono (1.0), stereo (2.0), stereo + LFE (2.1), 3.0 through 7.1.\n"
        "2.1 requires FL, FR, LFE input ordering; its compatibility core and lossless extension signal LFE.\n"
        "WAVEX/FLAC speaker layouts are validated; --channel-layout resolves ambiguous input.\n"
        "--lossy or --mode core selects the DCA core alone. --bitrate controls the core in both modes.\n"
        "--prediction-order N (alias --xll-max-prediction-order): maximum adaptive XLL order, 0..15; default 10.\n"
        "  Smaller orders may be chosen per channel; 0 disables adaptive prediction, retaining reference-compatible fixed order 0.\n"
        "  This controls lossless XLL only, not lossy DCA core ADPCM.\n"
        "--max-fixed-prediction-order 0: compatibility option; fixed orders 1..3 are not emitted because the reference encoder does not use them.\n"
        "--xll-parameter-reuse auto|on|off: permit or forbid reuse of prior segment entropy parameters; default auto.\n"
        "--xll-decorrelation auto|on|off: rate-select, always signal, or disable channel-pair decorrelation; default on (hardware PCM compatibility).\n"
        "--xll-max-segments 2|4|8|16: upper bound per XLL frame; default 16, subject to the sampling-rate segment ceiling.\n"
        "--xll-hardware-test: force a minimal compatibility stream: full-PCM XLL, raw linear coding, no prediction/decorrelation/reuse, 2 segments.\n"
        "  This diagnostic profile takes precedence over reference/hierarchy/ultra settings; use it only to isolate hardware decoder behavior.\n"
        "--adpcm enables optional DCA subband ADPCM in the lossy core (default: off).\n"
        "--core-lookahead aligns non-ADPCM 512-sample compatibility core to XLL for mono through 7.1 (default when supported); --no-core-lookahead restores legacy framing.\n"
        "7.0/7.1 uses 5.0/5.1 core; default --core-side both folds side+rear ONLY into the 5.x surrounds; FL/FR/FC/LFE stay unchanged.\n"
        "  front/rear/mute remain legacy diagnostic overrides; lossless XLL always preserves all original channels.\n"
        "Only the compatibility core is downmixed: lossless XLL preserves all original 7/8 channels.\n"
        "--xll-reference auto: compare full PCM, same-channel core residuals and a cheap mixed candidate; this is the default.\n"
        "For 24-bit/48-kHz/512-sample 7.1, the default XLL topology is an embedded 5.1 primary set plus Lsr/Rsr,\n"
        "  matching the supplied receiver-compatible reference structure. Its surround downmix is peak-safe.\n"
        "--xll-7x-hierarchy off forces the old single-set diagnostic form; on forces the stable two-set hierarchy;\n"
        "  normal 24-bit/512-sample 7.1 uses the stable reference-compatible hierarchy with aligned core residuals.\n"
        "  auto restores legacy topology comparison. --core-side affects only the compatibility-core downmix.\n"
        "--ultra additionally searches all legal reference masks and 24-bit MSB/LSB splitting; slower.\n"
        "--xll-frame-samples 512 (default) or 1024: XLL and core use matching genuine frame lengths.\n"
        "--xll-reference core: require core residual for every available core speaker; extra 7.x rear speakers remain full PCM.\n"
        "--threads N: XLL compression threads, 1..64; default=min(online CPUs,16).\n"
        "Progress: on by default; terminal stderr stays on one line; redirected stderr gets 10%% milestones.\n"
        "--simd auto (default) detects x86-64-v3; none forces portable C; unavailable explicit ISA fails.\n"
        "ratio=0.817 means raw codec bytes / processed PCM bytes (3 decimals, default).\n"
        "--ratio-container includes Matroska metadata and index, for comparison.\n"
        "--no-progress suppresses progress; --progress re-enables it.\n");
}
static int bitrate_parse(const char *arg,int*out) {
    if(!arg || !*arg || *arg<'0' || *arg>'9')return -1;
    errno=0;char *end=NULL;long val=strtol(arg,&end,10);
    if(errno || !end || *end || val>INT_MAX || val<0)return -1;
    for(size_t i=0;i<sizeof(bitrates)/sizeof(bitrates[0]);i++)
        if(val==bitrates[i]){*out=(int)val;return 0;}
    return -1;
}
static int pcm_positive_int(const char *arg,int *out){
    if(!arg || !*arg || *arg<'0' || *arg>'9')return -1;
    errno=0;char *end=NULL;long n=strtol(arg,&end,10);
    if(errno || !end || *end || n<1 || n>INT_MAX)return -1;
    *out=(int)n;return 0;
}
static int prediction_order_parse(const char *arg,int *out) {
    if (!arg || !*arg) return -1;
    for (const char *p=arg;*p;p++)
        if (*p<'0' || *p>'9') return -1;
    errno=0;
    char *end=NULL;
    long value=strtol(arg,&end,10);
    if (errno || !end || *end || value<0 || value>15) return -1;
    *out=(int)value;
    return 0;
}
static int fixed_prediction_order_parse(const char *arg,int *out) {
    int value=0;
    if(prediction_order_parse(arg,&value) || value!=0) return -1;
    *out=0;return 0;
}
static int max_segments_parse(const char *arg,int *out) {
    int value=0;
    if(pcm_positive_int(arg,&value) ||
       (value!=2 && value!=4 && value!=8 && value!=16)) return -1;
    *out=value;return 0;
}
static int threads_parse(const char *arg,unsigned *out) {
    if(!arg || !*arg || *arg<'0' || *arg>'9') return -1;
    errno=0;char *end=NULL;unsigned long n=strtoul(arg,&end,10);
    if(errno || !end || *end || n<1 || n>64)return -1;
    *out=(unsigned)n;return 0;
}
static const char *side_policy_name(HdmaSidePolicy policy)
{
    switch(policy) {
    case HDMA_SIDE_BOTH: return "both";
    case HDMA_SIDE_FRONT: return "front";
    case HDMA_SIDE_REAR: return "rear";
    case HDMA_SIDE_MUTE: return "mute";
    }
    return "invalid";
}

static int publish_job(HdmaWorkerPool *pool,uint64_t index,FILE *output,
                       MkaWriter *mux,int raw,int lossless,int channels,
                       uint64_t *residual_frames,uint64_t *full_frames,
                       uint64_t *mixed_frames,uint64_t *core_channels,
                       uint64_t *codec_bytes,HdmaProgress *progress)
{
    int result=hdma_pool_wait(pool,index);
    HdmaFrameJob *job=&pool->jobs[index%pool->slots];
    if(result<0) {
        hdma_progress_abort(progress);
        if(result==-2)fprintf(stderr,"XLL core residual exceeds PCM depth in frame %"PRIu64"; use --xll-reference auto\n",index);
        else fprintf(stderr,"XLL compression failed at frame %"PRIu64"\n",index);
        return -1;
    }
    if(lossless){
        if(job->used_core)(*residual_frames)++;
        else (*full_frames)++;
        if(job->used_core>0 && job->used_core<channels)(*mixed_frames)++;
        *core_channels+=(unsigned)job->used_core;
    }
    int rc;
    if(raw)
        rc=fwrite(job->packet,1,(size_t)job->packet_bytes,output)!=(size_t)job->packet_bytes;
    else if(lossless && job->got<pool->frame_samples)
        rc=mka_packet_trim(mux,job->packet,(size_t)job->packet_bytes,
                           index*(uint64_t)pool->frame_samples,pool->frame_samples-job->got);
    else
        rc=mka_packet(mux,job->packet,(size_t)job->packet_bytes,
                      index*(uint64_t)pool->frame_samples);
    if(rc){hdma_progress_abort(progress);fprintf(stderr,"output packet write failure at frame %"PRIu64"\n",index);return -1;}
    /* ftello includes the muxer header and all committed packet bytes even
     * while stdio output is buffered. A failed tell is an output failure. */
    off_t output_position=ftello(output);
    if(output_position<0){hdma_progress_abort(progress);perror("output position");return -1;}
    if (*codec_bytes > UINT64_MAX-(uint64_t)job->packet_bytes) {
        hdma_progress_abort(progress);
        fprintf(stderr,"raw codec byte count overflow\n"); return -1;
    }
    *codec_bytes+=(uint64_t)job->packet_bytes;
    uint64_t source_frames_done=job->source_frames_done;
    hdma_pool_release(pool,index);
    hdma_progress_advance(progress,source_frames_done,*codec_bytes,(uint64_t)output_position);
    return 0;
}
static char *default_target(const char *source) {
    const char *base=strrchr(source,'/');base=base?base+1:source;
    const char *dot=strrchr(base,'.');size_t n=dot?(size_t)(dot-source):strlen(source);
    if(n>SIZE_MAX-5)return NULL;
    char*out=malloc(n+5);if(!out)return NULL;
    memcpy(out,source,n);memcpy(out+n,".mka",5);return out;
}
static int suffix(const char*name,const char*ending) {
    size_t n=strlen(name),m=strlen(ending);
    return n>=m && strcmp(name+n-m,ending)==0;
}
static int same_file(const char*a,const char*b) {
    struct stat sa,sb;
    if(strcmp(a,b)==0)return 1;
    if(stat(a,&sa)==0 && stat(b,&sb)==0 && sa.st_dev==sb.st_dev && sa.st_ino==sb.st_ino)return 1;
    return 0;
}
static int pcm_rate(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_rate(r):pcm_file_input_rate(s);}
static int pcm_channels(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_channels(r):pcm_file_input_channels(s);}
static int pcm_depth(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_depth(r):pcm_file_input_depth(s);}
static int pcm_layout21(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_layout21(r):pcm_file_input_layout21(s);}
static int64_t pcm_frames(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_frames(r):pcm_file_input_frames(s);}
static int pcm_read(PcmFileInput *s,RawPcmInput *r,int32_t *p,int n){return r?raw_pcm_read(r,p,n):pcm_file_input_read(s,p,n);}
static const char *pcm_error(const PcmFileInput *s,const RawPcmInput *r){return r?raw_pcm_error(r):pcm_file_input_error(s);}
int main(int argc,char**argv) {
    int status=1,bitrate=1536000,overwrite=0,raw=0,lossless=1,fd=-1,show_progress=1,adpcm=0,core_lookahead=1;
    unsigned threads=hdma_default_thread_count(), inflight=0;
    uint64_t next_write=0,codec_bytes=0,predicted_subbands_total=0;
    int ratio_container=0, side_policy_explicit=0,ultra=0,frame_samples=512;
    int debug_core_melody=0, xll_policy_explicit=0, hierarchy_explicit=0,
        hierarchy_mode=HDMA_XLL_HIER_OFF, hardware_test=0;
    HdmaSidePolicy side_policy=HDMA_SIDE_BOTH;
    const char *simd_request="auto";
    HdmaWorkerPool pool={0};
    HdmaProgress progress={0};
    int xll_policy=HDMA_XLL_REF_AUTO;
    int max_prediction_order=10, max_fixed_prediction_order=0;
    int parameter_reuse=1, decorrelation_mode=HDMA_XLL_DECOR_ON, max_segments=16, force_raw=0;
    uint64_t residual_frames=0,full_frames=0, mixed_frames=0, core_channels=0;
    int64_t frames_read=0;
    uint64_t frame_number=0;
    const char *input_path=NULL,*target_path=NULL,*layout_override=NULL;
    char *derived=NULL,*temporary=NULL;
    PcmFileInput *input=NULL;
    RawPcmInput *raw_input=NULL;
    int raw_pcm=0,pcm_rate_hz=0,pcm_channels_count=0,pcm_depth_bits=0;
    DcaCore *codec=NULL;
    FILE *output=NULL;
    MkaWriter mux={0};
    if(argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"-h"))){help(stdout);return 0;}
    if(argc==2 && !strcmp(argv[1],"--version")){puts("hdmaenc 0.0.49-PUBLIC-NAMES-CLEANUP-TEST");return 0;}
    int input_arg=(argc>1 && !strcmp(argv[1],"encode")) ? 2 : 1;
    if(argc<=input_arg){help(stderr);return 2;}
    input_path=argv[input_arg];
    for(int i=input_arg+1;i<argc;i++) {
        if(!strcmp(argv[i],"-o") || !strcmp(argv[i],"--output")) {
            if(++i>=argc || target_path){fprintf(stderr,"invalid or duplicate output option\n");return 2;}
            target_path=argv[i];
        } else if(!strcmp(argv[i],"--raw-pcm"))raw_pcm=1;
        else if(!strcmp(argv[i],"--sample-rate")){
            if(++i>=argc){fprintf(stderr,"missing --sample-rate\n");return 2;}
            if(pcm_positive_int(argv[i],&pcm_rate_hz)){fprintf(stderr,"invalid --sample-rate\n");return 2;}
        } else if(!strcmp(argv[i],"--channels")){
            if(++i>=argc){fprintf(stderr,"missing --channels\n");return 2;}
            if(pcm_positive_int(argv[i],&pcm_channels_count)){fprintf(stderr,"invalid --channels\n");return 2;}
        } else if(!strcmp(argv[i],"--bit-depth")){
            if(++i>=argc){fprintf(stderr,"missing --bit-depth\n");return 2;}
            if(pcm_positive_int(argv[i],&pcm_depth_bits)){fprintf(stderr,"invalid --bit-depth\n");return 2;}
        } else if(!strcmp(argv[i],"--bitrate")) {
            if(++i>=argc || bitrate_parse(argv[i],&bitrate)){
                fprintf(stderr,"invalid DCA core bitrate: choose one of the standard rates\n");return 2;
            }
        } else if(!strcmp(argv[i],"--prediction-order") ||
                  !strcmp(argv[i],"--xll-max-prediction-order")) {
            if(++i>=argc || prediction_order_parse(argv[i],&max_prediction_order)){
                fprintf(stderr,"--prediction-order must be an integer from 0 to 15\n");return 2;
            }
        } else if(!strcmp(argv[i],"--max-fixed-prediction-order") ||
                  !strcmp(argv[i],"--xll-max-fixed-prediction-order")) {
            if(++i>=argc || fixed_prediction_order_parse(argv[i],&max_fixed_prediction_order)){
                fprintf(stderr,"--max-fixed-prediction-order only accepts 0; fixed orders 1..3 are disabled for reference compatibility\n");return 2;
            }
        } else if(!strcmp(argv[i],"--xll-parameter-reuse")) {
            if(++i>=argc){fprintf(stderr,"missing --xll-parameter-reuse value\n");return 2;}
            if(!strcmp(argv[i],"auto") || !strcmp(argv[i],"on")) parameter_reuse=1;
            else if(!strcmp(argv[i],"off")) parameter_reuse=0;
            else {fprintf(stderr,"--xll-parameter-reuse must be auto, on, or off\n");return 2;}
        } else if(!strcmp(argv[i],"--xll-decorrelation")) {
            if(++i>=argc){fprintf(stderr,"missing --xll-decorrelation value\n");return 2;}
            if(!strcmp(argv[i],"auto")) decorrelation_mode=HDMA_XLL_DECOR_AUTO;
            else if(!strcmp(argv[i],"on")) decorrelation_mode=HDMA_XLL_DECOR_ON;
            else if(!strcmp(argv[i],"off")) decorrelation_mode=HDMA_XLL_DECOR_OFF;
            else {fprintf(stderr,"--xll-decorrelation must be auto, on, or off\n");return 2;}
        } else if(!strcmp(argv[i],"--xll-max-segments")) {
            if(++i>=argc || max_segments_parse(argv[i],&max_segments)){
                fprintf(stderr,"--xll-max-segments must be 2, 4, 8, or 16\n");return 2;
            }
        } else if(!strcmp(argv[i],"--xll-hardware-test")) {
            hardware_test=1;
            max_prediction_order=0;
            max_fixed_prediction_order=0;
            parameter_reuse=0;
            decorrelation_mode=HDMA_XLL_DECOR_OFF;
            max_segments=2;
            force_raw=1;
        } else if(!strcmp(argv[i],"--threads")) {
            if(++i>=argc || threads_parse(argv[i],&threads)){
                fprintf(stderr,"--threads must be an integer from 1 to 64\n");return 2;
            }
        } else if(!strcmp(argv[i],"--xll-frame-samples") ||
                  !strcmp(argv[i],"--frame-samples")) {
            if(++i>=argc || (strcmp(argv[i],"512") && strcmp(argv[i],"1024"))) {
                fprintf(stderr,"--xll-frame-samples must be 512 or 1024\n");return 2;
            }
            frame_samples=atoi(argv[i]);
        } else if(!strcmp(argv[i],"--simd")) {
            if(++i>=argc){fprintf(stderr,"missing --simd value\n");return 2;}
            simd_request=argv[i];
        } else if(!strcmp(argv[i],"--ratio-container"))ratio_container=1;
        else if(!strcmp(argv[i],"--ratio-codec"))ratio_container=0;
        else if(!strcmp(argv[i],"--core-side") || !strcmp(argv[i],"--side-core")) {
            if(++i>=argc || side_policy_explicit){
                fprintf(stderr,"missing or duplicate --core-side policy\n");return 2;
            }
            side_policy_explicit=1;
            if(!strcmp(argv[i],"both")) side_policy=HDMA_SIDE_BOTH;
            else if(!strcmp(argv[i],"front")) side_policy=HDMA_SIDE_FRONT;
            else if(!strcmp(argv[i],"rear")) side_policy=HDMA_SIDE_REAR;
            else if(!strcmp(argv[i],"mute")) side_policy=HDMA_SIDE_MUTE;
            else {fprintf(stderr,"--core-side must be both, front, rear or mute\n");return 2;}
        }
        else if(!strcmp(argv[i],"--channel-layout")) {
            if(++i>=argc || layout_override){fprintf(stderr,"invalid or duplicate layout option\n");return 2;}
            layout_override=argv[i];
        } else if(!strcmp(argv[i],"--xll-reference")) {
            if(++i>=argc){fprintf(stderr,"missing XLL reference policy\n");return 2;}
            xll_policy_explicit=1;
            if(!strcmp(argv[i],"auto"))xll_policy=HDMA_XLL_REF_AUTO;
            else if(!strcmp(argv[i],"full"))xll_policy=HDMA_XLL_REF_FULL;
            else if(!strcmp(argv[i],"core"))xll_policy=HDMA_XLL_REF_CORE;
            else {fprintf(stderr,"XLL reference must be auto, full, or core\n");return 2;}
        } else if(!strcmp(argv[i],"--xll-7x-hierarchy")) {
            if(++i>=argc){fprintf(stderr,"missing --xll-7x-hierarchy value\n");return 2;}
            hierarchy_explicit=1;
            if(!strcmp(argv[i],"off")) hierarchy_mode=HDMA_XLL_HIER_OFF;
            else if(!strcmp(argv[i],"on")) hierarchy_mode=HDMA_XLL_HIER_ON;
            else if(!strcmp(argv[i],"auto")) hierarchy_mode=HDMA_XLL_HIER_AUTO;
            else {fprintf(stderr,"--xll-7x-hierarchy must be off, on, or auto\n");return 2;}
        } else if(!strcmp(argv[i],"--ultra"))ultra=1;
        else if(!strcmp(argv[i],"--debug-core-melody"))debug_core_melody=1;
        else if(!strcmp(argv[i],"--core-lookahead"))core_lookahead=1;
        else if(!strcmp(argv[i],"--no-core-lookahead"))core_lookahead=0;
        else if(!strcmp(argv[i],"--overwrite"))overwrite=1;
        else if(!strcmp(argv[i],"--no-progress"))show_progress=0;
        else if(!strcmp(argv[i],"--progress"))show_progress=1,adpcm=0;
        else if(!strcmp(argv[i],"--lossless"))lossless=1;
        else if(!strcmp(argv[i],"--lossy"))lossless=0;
        else if(!strcmp(argv[i],"--adpcm"))adpcm=1;
        else if(!strcmp(argv[i],"--no-adpcm"))adpcm=0;
        else if(!strcmp(argv[i],"--mode") && i+1<argc){
            const char *mode=argv[++i];
            if(!strcmp(mode,"lossless") || !strcmp(mode,"hdma"))lossless=1;
            else if(!strcmp(mode,"core"))lossless=0;
            else {fprintf(stderr,"unsupported mode: %s\n",mode);return 2;}
        } else {fprintf(stderr,"unknown argument: %s\n",argv[i]);return 2;}
    }
    int simd_result=hdma_simd_configure(simd_request);
    if(simd_result){
        fprintf(stderr,simd_result==-2 ? "SIMD backend unavailable on this CPU or build: %s\n" :
                "unknown SIMD backend: %s (choose auto, none or x86-64-v3)\n",simd_request);
        return 2;
    }
    if(hardware_test && !lossless) {
        fprintf(stderr,"--xll-hardware-test requires lossless output\n");return 2;
    }
    if(!lossless && xll_policy!=HDMA_XLL_REF_AUTO) {
        fprintf(stderr,"--xll-reference requires lossless output\n");return 2;
    }
    if(debug_core_melody && (!lossless ||
        (xll_policy_explicit && xll_policy==HDMA_XLL_REF_CORE))){
        fprintf(stderr,"--debug-core-melody requires lossless full-PCM XLL; incompatible with --lossy and --xll-reference core\n");
        return 2;
    }
    if(debug_core_melody){
        /* Core residuals against an intentionally different song would not
         * leave the HD program independent of the diagnostic. Use original
         * input for XLL and force no core dependencies, even in auto mode. */
        xll_policy=HDMA_XLL_REF_FULL;
        core_lookahead=0;
        fprintf(stderr,"WARNING: --debug-core-melody active: synthetic low-level melody in DCA core ONLY; original PCM in full lossless XLL. Do not distribute this disc.\n");
    }
    if(!target_path){if(!strcmp(input_path,"-")){fprintf(stderr,"stdin input requires -o OUTPUT.dca\n");goto cleanup;}derived=default_target(input_path);target_path=derived;}
    if(!target_path || !*target_path){fprintf(stderr,"out of memory or invalid target\n");goto cleanup;}
    raw=suffix(target_path,".dca") || suffix(target_path,".dts");
    if(!raw && !suffix(target_path,".mka")){
        fprintf(stderr,"output must end in .mka (default), .dca, or .dts (compatibility)\n");goto cleanup;
    }
    if(strcmp(input_path,"-") && same_file(input_path,target_path)){
        fprintf(stderr,"input and output paths must differ\n");goto cleanup;
    }
    if(!overwrite && access(target_path,F_OK)==0){
        fprintf(stderr,"output already exists; use --overwrite: %s\n",target_path);goto cleanup;
    }
    if(raw_pcm){
        if(raw_pcm_open(&raw_input,input_path,pcm_rate_hz,pcm_channels_count,pcm_depth_bits,layout_override))goto cleanup;
    }else if(pcm_file_input_open(&input,input_path,layout_override)){
        fprintf(stderr,"cannot open supported libsndfile input: %s\n",input_path);goto cleanup;
    }
    const int64_t source_frames_expected=pcm_frames(input,raw_input);
    int channels=pcm_channels(input,raw_input),rate=pcm_rate(input,raw_input);
    int encoded_channels=channels>6 ? (channels==7 ? 5 : 6) : channels;
    const int layout21=pcm_layout21(input,raw_input);
    if(lossless) {
        int segment_ceiling=rate<=48000 ? 256 : 512;
        int minimum_segments=(frame_samples+segment_ceiling-1)/segment_ceiling;
        if(max_segments<minimum_segments) {
            fprintf(stderr,"--xll-max-segments %d is too small for %d samples/frame at %d Hz; minimum is %d\n",
                    max_segments,frame_samples,rate,minimum_segments);
            goto cleanup;
        }
    }
    if(channels<=6 && side_policy_explicit){
        fprintf(stderr,"--core-side applies only to 7.0/7.1 sources\n");goto cleanup;
    }
    if(hardware_test) {
        xll_policy=HDMA_XLL_REF_FULL;
        hierarchy_mode=HDMA_XLL_HIER_OFF;
        ultra=0;
        fprintf(stderr,"XLL hardware test: full-PCM single-set raw linear coding; predictor/decorrelation/reuse disabled\n");
    }
    /* Core/XLL lookahead is the normal lossless path.  Do not infer a full-XLL
     * / legacy-alignment request merely because the caller spelled out a
     * default-valued option.  In particular, --core-side both and
     * --xll-reference auto are exactly equivalent to omitting those options.
     *
     * The supplied receiver-compatible 24-bit/512-sample 7.1 reference uses
     * the stable two-set hierarchy.  Select it whenever the caller has not
     * explicitly selected another topology and the core downmix is the
     * reference-compatible BOTH policy.  Alternate side policies remain
     * lossless and aligned, but use the single-set XLL representation because
     * the reference hierarchy's embedded matrix is fixed to BOTH. */
    int alternate_side_policy = side_policy!=HDMA_SIDE_BOTH;
    if(lossless && channels==8 && pcm_depth(input,raw_input)==24 && frame_samples==512 &&
       !hardware_test && !hierarchy_explicit && !alternate_side_policy &&
       xll_policy!=HDMA_XLL_REF_FULL) {
        hierarchy_mode=HDMA_XLL_HIER_ON;
        fprintf(stderr,"7.1 reference-compatible default: core-residual 5.1 primary XLL + Lsr/Rsr extension\n");
    }
    if(lossless && channels>6 && alternate_side_policy && hierarchy_explicit &&
       hierarchy_mode!=HDMA_XLL_HIER_OFF && xll_policy!=HDMA_XLL_REF_FULL) {
        fprintf(stderr,"--xll-7x-hierarchy on/auto is incompatible with --core-side front/rear/mute when core-referenced XLL is enabled; use --xll-7x-hierarchy off or --xll-reference full\n");
        goto cleanup;
    }
    if((frame_samples==1024 ? dca_core_create_layout_1024 : dca_core_create_layout)
       (&codec,encoded_channels,rate,bitrate,layout21)){
        fprintf(stderr,"unsupported DCA core sample-rate/channel-count/bitrate combination: %d Hz, %d channels, %d b/s\n",
                rate,channels,bitrate);goto cleanup;
    }
    if ((frame_samples==1024 ? dca_core_set_adpcm_1024 : dca_core_set_adpcm)(codec,adpcm)) {
        fprintf(stderr,"could not configure core ADPCM\n");goto cleanup;
    }
    if ((frame_samples==1024 ? dca_core_set_source_depth_1024 : dca_core_set_source_depth)
        (codec,pcm_depth(input,raw_input))) {
        fprintf(stderr,"could not configure core source depth\n");goto cleanup;
    }
    size_t templen=strlen(target_path)+sizeof(".hdmaenc-XXXXXX");
    temporary=malloc(templen);if(!temporary){fprintf(stderr,"out of memory\n");goto cleanup;}
    snprintf(temporary,templen,"%s.hdmaenc-XXXXXX",target_path);
    fd=mkstemp(temporary);if(fd<0){perror("create staged output");goto cleanup;}
    output=fdopen(fd,"wb");if(!output){perror("fdopen");goto cleanup;}fd=-1;
    const char *legacy_alignment_reason=NULL;
    int aligned_core=0;
    if(lossless) {
        if(!core_lookahead) legacy_alignment_reason="disabled by --no-core-lookahead";
        else if(adpcm) legacy_alignment_reason="incompatible with --adpcm";
        else if(frame_samples!=512) legacy_alignment_reason="currently unsupported with 1024-sample frames";
        else if(xll_policy==HDMA_XLL_REF_FULL) legacy_alignment_reason=
            hardware_test ? "--xll-hardware-test forces full-PCM XLL" :
            debug_core_melody ? "--debug-core-melody requires independent full-PCM XLL" :
            "--xll-reference full does not reference the DCA core";
        else if(layout21) legacy_alignment_reason="2.1 XLL currently uses an independent full-PCM extension";
        else if(channels>8) legacy_alignment_reason="core lookahead supports at most 8 source channels";
        else aligned_core=1;
        if(!aligned_core && core_lookahead && legacy_alignment_reason)
            fprintf(stderr,"core alignment fallback: legacy (%s)\n",legacy_alignment_reason);
    }
    if(!raw && mka_start(&mux,output,rate,lossless ? channels : encoded_channels)){
        fprintf(stderr,"could not write Matroska headers\n");goto cleanup;
    }
    int bytes=(frame_samples==1024 ? dca_core_frame_bytes_1024 : dca_core_frame_bytes)(codec);
    if(bytes<=0 || bytes>16384){fprintf(stderr,"invalid internal frame size\n");goto cleanup;}
    /* AUTO remains a per-frame, per-channel cost/validity decision. A single
     * reference sample cannot establish a universal core-reference policy. */
    if(hdma_pool_init(&pool,threads,channels,rate,pcm_depth(input,raw_input),xll_policy,lossless,ultra,
                      frame_samples,(int)side_policy,layout21,hierarchy_mode,max_prediction_order,
                      max_fixed_prediction_order,parameter_reuse,decorrelation_mode,max_segments,force_raw)) {
        fprintf(stderr,"unable to start compression threads\n");goto cleanup;
    }
    hdma_progress_begin(&progress,source_frames_expected>0?(uint64_t)source_frames_expected:0,rate,channels,
                        pcm_depth(input,raw_input),show_progress,ratio_container,frame_samples);
    if(aligned_core) {
        int32_t pending[512*8], next_pcm[512*8], core_input[512*6];
        int32_t zero_core[512*6]={0};
        int pending_got=pcm_read(input,raw_input,pending,512);
        if(pending_got<0){hdma_progress_abort(&progress);fprintf(stderr,"PCM read failed: %s\n",pcm_error(input,raw_input));goto cleanup;}
        if(!pending_got){hdma_progress_abort(&progress);fprintf(stderr,"empty input\n");goto cleanup;}
        frames_read+=pending_got;
        if(pending_got<512)memset(pending+(size_t)pending_got*channels,0,
                                  ((size_t)512-pending_got)*channels*sizeof(int32_t));
        uint64_t source_frame_count=1;

        /* Prime the delayed core analysis/reconstruction state using the
         * same 5.x downmix and channel order as every emitted frame.
         * Do not emit the prime packet. The first emitted frame is full XLL;
         * subsequent frames may safely reference the now-synchronized core. */
        uint8_t hidden_packet[65536];
        const int32_t *core_source=pending;
        if(channels>6){
            int32_t primary_unused[512*6];
            int mapped=hdma_downmix_hierarchical_policy_n(pending,channels,
                primary_unused,core_input,side_policy,512,pcm_depth(input,raw_input));
            if(mapped!=encoded_channels){hdma_progress_abort(&progress);fprintf(stderr,"internal core downmix layout failure\n");goto cleanup;}
            core_source=core_input;
        }
        HdmaFrameJob *job=NULL;
        int written=dca_core_encode(codec,core_source,hidden_packet,sizeof(hidden_packet));
        if(written!=bytes){hdma_progress_abort(&progress);fprintf(stderr,"DCA encoder prime failure\n");goto cleanup;}
        /* Do not advance the core reconstruction filterbank for the hidden
         * analysis-prime packet. It is not transmitted, so an actual decoder
         * begins the first emitted core frame with a zero synthesis history.
         * Keeping our reference synthesizer in that same state lets frame 1
         * use a core XLL residual without sacrificing sample-exact losslessness. */
        int first_aligned_frame=1;

        for(;;) {
            int got=pcm_read(input,raw_input,next_pcm,512);
            if(got<0){hdma_progress_abort(&progress);fprintf(stderr,"PCM read failed: %s\n",pcm_error(input,raw_input));goto cleanup;}
            if(!got)break;
            frames_read+=got;
            if(got<512)memset(next_pcm+(size_t)got*channels,0,
                              ((size_t)512-got)*channels*sizeof(int32_t));
            if(inflight==pool.slots) {
                if(publish_job(&pool,next_write,output,&mux,raw,lossless,channels,
                    &residual_frames,&full_frames,&mixed_frames,&core_channels,&codec_bytes,&progress))goto cleanup;
                ++next_write;--inflight;
            }
            job=hdma_pool_slot(&pool,frame_number);
            if(!job){hdma_progress_abort(&progress);fprintf(stderr,"internal job queue failure\n");goto cleanup;}
            memcpy(job->pcm,pending,(size_t)512*channels*sizeof(int32_t));
            job->got=pending_got;
            /* Emit a self-contained startup frame: our hidden analysis prime
             * is not transmitted, so no decoder may depend on its history. */
            job->force_full=first_aligned_frame;
            job->source_frames_done=source_frame_count;
            core_source=next_pcm;
            if(channels>6){
                int32_t primary_unused[512*6];
                int mapped=hdma_downmix_hierarchical_policy_n(next_pcm,channels,
                    primary_unused,core_input,side_policy,512,pcm_depth(input,raw_input));
                if(mapped!=encoded_channels){hdma_progress_abort(&progress);fprintf(stderr,"internal core downmix layout failure\n");goto cleanup;}
                core_source=core_input;
            }
            written=dca_core_encode(codec,core_source,job->packet,sizeof(job->packet));
            if(written!=bytes){hdma_progress_abort(&progress);fprintf(stderr,"DCA encoder frame failure at frame %"PRIu64"\n",frame_number);goto cleanup;}
            job->core_bytes=written;
            if(adpcm)predicted_subbands_total+=(unsigned)dca_core_adpcm_bands(codec);
            if(dca_core_reconstruct(codec,job->core_pcm,(size_t)512*(unsigned)encoded_channels)){
                hdma_progress_abort(&progress);fprintf(stderr,"DCA core reconstruction failure at frame %"PRIu64"\n",frame_number);goto cleanup;
            }
            if(hdma_pool_submit(&pool,frame_number)){hdma_progress_abort(&progress);fprintf(stderr,"could not submit XLL job\n");goto cleanup;}
            ++inflight;++frame_number;
            first_aligned_frame=0;
            memcpy(pending,next_pcm,(size_t)512*channels*sizeof(int32_t));
            pending_got=got;++source_frame_count;
        }

        /* One zero look-ahead frame flushes the final source frame through
         * the core analysis/synthesis delay. This packet carries the final
         * source XLL residual and normal end DiscardPadding when needed. */
        if(inflight==pool.slots) {
            if(publish_job(&pool,next_write,output,&mux,raw,lossless,channels,
                &residual_frames,&full_frames,&mixed_frames,&core_channels,&codec_bytes,&progress))goto cleanup;
            ++next_write;--inflight;
        }
        job=hdma_pool_slot(&pool,frame_number);
        if(!job){hdma_progress_abort(&progress);fprintf(stderr,"internal job queue failure\n");goto cleanup;}
        memcpy(job->pcm,pending,(size_t)512*channels*sizeof(int32_t));
        job->got=pending_got;job->force_full=first_aligned_frame;
        job->source_frames_done=source_frame_count;
        written=dca_core_encode(codec,zero_core,job->packet,sizeof(job->packet));
        if(written!=bytes){hdma_progress_abort(&progress);fprintf(stderr,"DCA encoder flush failure\n");goto cleanup;}
        job->core_bytes=written;
        if(adpcm)predicted_subbands_total+=(unsigned)dca_core_adpcm_bands(codec);
        if(dca_core_reconstruct(codec,job->core_pcm,(size_t)512*(unsigned)encoded_channels)){
            hdma_progress_abort(&progress);fprintf(stderr,"DCA core flush reconstruction failure\n");goto cleanup;
        }
        if(hdma_pool_submit(&pool,frame_number)){hdma_progress_abort(&progress);fprintf(stderr,"could not submit final XLL job\n");goto cleanup;}
        ++inflight;++frame_number;
    } else {
        for(;;) {
            /* Bound live work to two frames per thread. The core is sequential;
             * XLL is independent for each matching core/XLL frame. Never seek or write
             * out of order, even if a later XLL frame completes first. */
            if(inflight==pool.slots) {
                if(publish_job(&pool,next_write,output,&mux,raw,lossless,channels,
                    &residual_frames,&full_frames,&mixed_frames,&core_channels,&codec_bytes,&progress))goto cleanup;
                ++next_write;--inflight;
            }
            HdmaFrameJob *job=hdma_pool_slot(&pool,frame_number);
            if(!job){hdma_progress_abort(&progress);fprintf(stderr,"internal job queue failure\n");goto cleanup;}
            int got=pcm_read(input,raw_input,job->pcm,512);
            if(got==512 && frame_samples==1024) {
                int second=pcm_read(input,raw_input,job->pcm+(size_t)512*channels,512);
                if(second<0){hdma_progress_abort(&progress);fprintf(stderr,"PCM read failed: %s\n",pcm_error(input,raw_input));goto cleanup;}
                got+=second;
            }
            if(got<0){hdma_progress_abort(&progress);fprintf(stderr,"PCM read failed: %s\n",pcm_error(input,raw_input));goto cleanup;}
            if(!got)break;
            frames_read+=got;
            job->got=got;job->force_full=0;job->source_frames_done=frame_number+1;
            if(got<frame_samples)memset(job->pcm+(size_t)got*channels,0,
                              ((size_t)frame_samples-got)*channels*sizeof(int32_t));
            int32_t core_input[1024*6];
            const int32_t *core_source=job->pcm;
            if(channels>6){
                /* Lossless 7.x uses the exact reversible hierarchy signalled in
                 * XLL. Lossy-only output retains the historical direct downmix. */
                int mapped;
                if (lossless) {
                    int32_t primary_unused[1024*6];
                    mapped=hdma_downmix_hierarchical_policy_n(job->pcm,channels,
                           primary_unused,core_input,side_policy,frame_samples,pcm_depth(input,raw_input));
                } else {
                    mapped=hdma_downmix_core_policy(job->pcm,channels,core_input,side_policy);
                    if(mapped==encoded_channels && frame_samples==1024)
                        mapped=hdma_downmix_core_policy(job->pcm+(size_t)512*channels,
                               channels,core_input+(size_t)512*encoded_channels,side_policy);
                }
                if(mapped!=encoded_channels) {
                    hdma_progress_abort(&progress);
                    fprintf(stderr,"internal core downmix layout failure\n"); goto cleanup;
                }
                core_source=core_input;
            }
            if(debug_core_melody){
                if(hdma_core_signature(core_input,frame_samples,encoded_channels,rate,
                                       pcm_depth(input,raw_input),layout21,frame_number)){
                    hdma_progress_abort(&progress);
                    fprintf(stderr,"internal diagnostic motif generation failure\n");goto cleanup;
                }
                core_source=core_input;
            }
            int written=(frame_samples==1024 ? dca_core_encode_1024 : dca_core_encode)
                             (codec,core_source,job->packet,sizeof(job->packet));
            if(written!=bytes){hdma_progress_abort(&progress);fprintf(stderr,"DCA encoder frame failure at frame %"PRIu64"\n",frame_number);goto cleanup;}
            job->core_bytes=written;
            if(adpcm)predicted_subbands_total+=(unsigned)(frame_samples==1024 ?
                dca_core_adpcm_bands_1024(codec) : dca_core_adpcm_bands(codec));
            if(lossless && xll_policy!=HDMA_XLL_REF_FULL){
                int32_t *target=job->core_pcm;
                if((frame_samples==1024 ? dca_core_reconstruct_1024 : dca_core_reconstruct)
                   (codec,target,(size_t)frame_samples*(unsigned)encoded_channels)){
                    hdma_progress_abort(&progress);
                    fprintf(stderr,"DCA core reconstruction failure at frame %"PRIu64"\n",frame_number);
                    goto cleanup;
                }
            }
            if(hdma_pool_submit(&pool,frame_number)){
                hdma_progress_abort(&progress);
                fprintf(stderr,"could not submit XLL job\n");goto cleanup;
            }
            ++inflight;++frame_number;
        }
    }
    while(inflight){
        if(publish_job(&pool,next_write,output,&mux,raw,lossless,channels,
            &residual_frames,&full_frames,&mixed_frames,&core_channels,&codec_bytes,&progress))goto cleanup;
        ++next_write;--inflight;
    }
    if((source_frames_expected>=0 && frames_read!=source_frames_expected) || frame_number==0){
        hdma_progress_abort(&progress);fprintf(stderr,"PCM sample count mismatch or empty input\n");goto cleanup;
    }
    if((!raw && mka_finish(&mux, lossless ? (uint64_t)frames_read : frame_number*(uint64_t)frame_samples)) ||
       fflush(output) || ferror(output)){
        hdma_progress_abort(&progress);fprintf(stderr,"output flush failed\n");goto cleanup;
    }
    off_t final_position=ftello(output);
    if(final_position<0){hdma_progress_abort(&progress);perror("output position");goto cleanup;}
    if(fclose(output)){output=NULL;hdma_progress_abort(&progress);perror("output close");goto cleanup;}output=NULL;
    if(overwrite){
        if(rename(temporary,target_path)){hdma_progress_abort(&progress);perror("publish output");goto cleanup;}
    } else {
        if(link(temporary,target_path)){hdma_progress_abort(&progress);perror("publish output (will not overwrite)");goto cleanup;}
        unlink(temporary);
    }
    hdma_progress_finish(&progress,codec_bytes,(uint64_t)final_position);
    printf("Wrote %s: %"PRIu64" %s frames, %"PRId64" source samples/channel, %d Hz, %d output channels, %d b/s nominal core\n",
           target_path,frame_number,lossless?"DCA core + compressed lossless XLL":"lossy DCA",frames_read,rate,
           lossless ? channels : encoded_channels,bitrate);
    if (channels>6) printf("Core downmix: %d-channel source -> %d-channel DCA core (side policy=%s); %s\n",
                           channels,encoded_channels,side_policy_name(side_policy),
                           lossless ? "XLL preserves all original channels, unchanged" : "lossy-only output");
    if(lossless) printf("XLL prediction: %"PRIu64" core-referenced frames, %"PRIu64" full-PCM frames (policy=%s)\n",
        residual_frames,full_frames,xll_policy==HDMA_XLL_REF_CORE?"core":
        xll_policy==HDMA_XLL_REF_FULL?"full":"auto");
    if(lossless)printf("XLL tuning: adaptive-order<=%d fixed-order<=%d parameter-reuse=%s decorrelation=%s max-segments=%d raw-linear=%s\n",
        max_prediction_order,max_fixed_prediction_order,parameter_reuse?"auto":"off",
        decorrelation_mode==HDMA_XLL_DECOR_AUTO?"auto":decorrelation_mode==HDMA_XLL_DECOR_ON?"on":"off",
        max_segments,force_raw?"yes":"no");
    if(lossless)printf("XLL reference detail: %"PRIu64" mixed-reference frames; %"PRIu64" core-referenced channel-frames\n",
                       mixed_frames,core_channels);
    if(lossless && channels==8 && pcm_depth(input,raw_input)==24 && frame_samples==512)
        printf("7.1 XLL topology: %s\n", hierarchy_mode==HDMA_XLL_HIER_OFF ?
               "single-set (explicit diagnostic override)" : hierarchy_mode==HDMA_XLL_HIER_ON ?
               "embedded 5.1 primary + Lsr/Rsr extension (stable)" :
               "legacy per-frame auto-switching (diagnostic)");
    if(lossless) {
        printf("XLL compression threads: %u; ultra: %s; core alignment: %s",
               pool.worker_count ? pool.worker_count : 1U,ultra?"on":"off",
               aligned_core?"one-frame lookahead":"legacy");
        if(!aligned_core && legacy_alignment_reason) printf(" (%s)",legacy_alignment_reason);
        putchar('\n');
    }
    if(debug_core_melody)printf("DEBUG CORE MELODY: synthetic chime in lossy core; original source PCM in independent full lossless XLL; NOT for release discs\n");
    if(adpcm)printf("DCA core ADPCM: %"PRIu64" predicted subbands across %"PRIu64" frames\n",
                    predicted_subbands_total,frame_number);
    printf("Core ADPCM: %s; frame samples: %d; SIMD backend: %s; raw codec: %"PRIu64" bytes; container: %"PRIu64" bytes\n",
           adpcm ? "on" : "off",frame_samples,hdma_simd_selected(),codec_bytes,(uint64_t)final_position);
    status=0;
cleanup:
    hdma_progress_abort(&progress);
    hdma_pool_destroy(&pool);
    mka_dispose(&mux);
    if(output)fclose(output);
    if(fd>=0)close(fd);
    if(status && temporary)unlink(temporary);
    (frame_samples==1024 ? dca_core_destroy_1024 : dca_core_destroy)(codec);
    pcm_file_input_close(input);
    raw_pcm_close(raw_input);
    free(temporary);free(derived);
    return status;
}
