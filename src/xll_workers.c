/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Only XLL is parallel: the stateful DCA core encoder/reconstructor runs
 * serially in the producer, and the caller muxes frames in original order.
 */
#include "xll_workers.h"
#include "xll.h"
#include <stdlib.h>
#include <string.h>

static void process_frame(HdmaWorkerPool *p, HdmaFrameJob *job)
{
    int written=job->core_bytes;
    job->result=0;
    job->used_core=0;
    if (p->lossless) {
        /* Set the cap in the actual encoding thread (also used for --threads 1).
         * Each frame resets it, including when pool threads are reused. */
        int configured=(p->frame_samples==1024 ? hdma_xll_set_max_prediction_order_1024
                       : hdma_xll_set_max_prediction_order)(p->max_prediction_order);
        configured |= (p->frame_samples==1024 ? hdma_xll_set_max_fixed_prediction_order_1024
                       : hdma_xll_set_max_fixed_prediction_order)(p->max_fixed_prediction_order);
        configured |= (p->frame_samples==1024 ? hdma_xll_set_parameter_reuse_1024
                       : hdma_xll_set_parameter_reuse)(p->parameter_reuse);
        configured |= (p->frame_samples==1024 ? hdma_xll_set_decorrelation_mode_1024
                       : hdma_xll_set_decorrelation_mode)(p->decorrelation_mode);
        configured |= (p->frame_samples==1024 ? hdma_xll_set_max_segments_1024
                       : hdma_xll_set_max_segments)(p->max_segments);
        configured |= (p->frame_samples==1024 ? hdma_xll_set_force_raw_1024
                       : hdma_xll_set_force_raw)(p->force_raw);
        if (configured) { job->result=-1; return; }
        int aligned=(written+3)&~3;
        if (aligned >= (int)HDMA_JOB_CAPACITY) { job->result=-1; return; }
        memset(job->packet+written,0,(size_t)(aligned-written));
        int extension;
        if (p->layout21) {
            /* The 2.1 core has a real LFE, and XLL must signal FL/FR/LFE,
             * not the three-full-band 3.0 default. Full XLL is exact. */
            extension=(p->frame_samples==1024 ? hdma_xll_encode_21_1024 : hdma_xll_encode_21)(
                job->pcm,p->rate,p->depth,job->packet+aligned,HDMA_JOB_CAPACITY-(size_t)aligned);
        } else if (p->channels > 6) {
            /* Keep the first emitted 24-bit/512-sample 7.1 hierarchy
             * self-contained. The hidden analysis prime is not transmitted and
             * independent decoders enter recovery on a multi-set residual first
             * frame. Hardware-oriented common-header syntax is matched without
             * weakening sample-0 losslessness. */
            int frame_mode = job->force_full ? HDMA_XLL_REF_FULL :
                ((job->index == 0 && p->channels == 8 && p->depth == 24 &&
                  p->frame_samples == 512) ? HDMA_XLL_REF_FULL : p->mode);
            extension=(p->frame_samples==1024 ? hdma_xll_encode_hierarchical_7x_policy_options_1024
                       : hdma_xll_encode_hierarchical_7x_policy_options)(job->pcm,
                frame_mode==HDMA_XLL_REF_FULL ? NULL : job->core_pcm,
                p->channels,p->rate,p->depth,job->packet+aligned,
                HDMA_JOB_CAPACITY-(size_t)aligned,frame_mode,&job->used_core,p->ultra,p->side_policy,p->hierarchy_mode);
        } else {
            int frame_mode = job->force_full ? HDMA_XLL_REF_FULL : p->mode;
            extension=(p->frame_samples==1024 ? hdma_xll_encode_core_mode_options_1024
                       : hdma_xll_encode_core_mode_options)(job->pcm,
                frame_mode==HDMA_XLL_REF_FULL ? NULL : job->core_pcm,
                p->channels,p->rate,p->depth,job->packet+aligned,
                HDMA_JOB_CAPACITY-(size_t)aligned,frame_mode,&job->used_core,p->ultra);
        }
        if (extension<0) {job->result=extension;return;}
        written=aligned+extension;
    }
    job->packet_bytes=written;
}

static void *worker_main(void *arg)
{
    HdmaWorkerPool *p=arg;
    for (;;) {
        HdmaFrameJob *job=NULL;
        pthread_mutex_lock(&p->mutex);
        while (!p->stopping && !job) {
            for (unsigned i=0;i<p->slots;i++) {
                if (p->jobs[i].state==1) {
                    job=&p->jobs[i];job->state=2;break;
                }
            }
            if (!job) pthread_cond_wait(&p->ready,&p->mutex);
        }
        if (p->stopping) {pthread_mutex_unlock(&p->mutex);break;}
        pthread_mutex_unlock(&p->mutex);
        process_frame(p,job);
        pthread_mutex_lock(&p->mutex);
        job->state=3;
        pthread_cond_broadcast(&p->complete);
        pthread_mutex_unlock(&p->mutex);
    }
    return NULL;
}

int hdma_pool_init(HdmaWorkerPool *p,unsigned threads,int channels,int rate,
                   int depth,int mode,int lossless,int ultra,int frame_samples,int side_policy,
                   int layout21,int hierarchy_mode,int max_prediction_order,int max_fixed_prediction_order,
                   int parameter_reuse,int decorrelation_mode,int max_segments,int force_raw)
{
    if (!p || !threads || threads>64 || channels<1 || channels>8 ||
        (frame_samples!=512 && frame_samples!=1024) ||
        (layout21 && channels!=3) || hierarchy_mode<HDMA_XLL_HIER_OFF || hierarchy_mode>HDMA_XLL_HIER_AUTO ||
        max_prediction_order<0 || max_prediction_order>15 ||
        max_fixed_prediction_order!=0 ||
        (parameter_reuse!=0 && parameter_reuse!=1) ||
        decorrelation_mode<HDMA_XLL_DECOR_AUTO || decorrelation_mode>HDMA_XLL_DECOR_ON ||
        (max_segments!=2 && max_segments!=4 && max_segments!=8 && max_segments!=16) ||
        (force_raw!=0 && force_raw!=1)) return -1;
    memset(p,0,sizeof(*p));
    p->channels=channels;p->rate=rate;p->depth=depth;p->mode=mode;
    p->lossless=lossless;p->ultra=ultra;p->frame_samples=frame_samples;p->side_policy=side_policy;p->layout21=layout21;p->hierarchy_mode=hierarchy_mode;
    p->max_prediction_order=max_prediction_order;p->max_fixed_prediction_order=max_fixed_prediction_order;
    p->parameter_reuse=parameter_reuse;p->decorrelation_mode=decorrelation_mode;p->max_segments=max_segments;p->force_raw=force_raw;
    /* Keep the lossy-only path free of needless thread creation. */
    p->worker_count=lossless && threads>1 ? threads : 0;
    p->slots=p->worker_count ? 2U*threads : 1;
    p->jobs=calloc(p->slots,sizeof(*p->jobs));
    if (!p->jobs) return -1;
    if (pthread_mutex_init(&p->mutex,NULL)) goto fail_jobs;
    if (pthread_cond_init(&p->ready,NULL)) goto fail_mutex;
    if (pthread_cond_init(&p->complete,NULL)) goto fail_ready;
    p->initialized=1;
    if (p->worker_count) {
        p->workers=calloc(p->worker_count,sizeof(*p->workers));
        if (!p->workers) goto fail;
        for (unsigned i=0;i<p->worker_count;i++) {
            if (pthread_create(&p->workers[i],NULL,worker_main,p)) goto fail;
            p->started++;
        }
    }
    return 0;
fail:
    hdma_pool_destroy(p);return -1;
fail_ready:
    pthread_cond_destroy(&p->ready);
fail_mutex:
    pthread_mutex_destroy(&p->mutex);
fail_jobs:
    free(p->jobs);p->jobs=NULL;return -1;
}
HdmaFrameJob *hdma_pool_slot(HdmaWorkerPool *p,uint64_t index)
{
    if (!p || !p->jobs) return NULL;
    HdmaFrameJob *job=&p->jobs[index%p->slots];
    return job->state==0 ? job : NULL;
}
int hdma_pool_submit(HdmaWorkerPool *p,uint64_t index)
{
    HdmaFrameJob *job=hdma_pool_slot(p,index);
    if (!job) return -1;
    job->index=index;
    if (!p->worker_count) {
        job->state=2;
        process_frame(p,job);
        job->state=3;
        return 0;
    }
    pthread_mutex_lock(&p->mutex);
    job->state=1;
    pthread_cond_signal(&p->ready);
    pthread_mutex_unlock(&p->mutex);
    return 0;
}
int hdma_pool_wait(HdmaWorkerPool *p,uint64_t index)
{
    if (!p || !p->jobs) return -1;
    HdmaFrameJob *job=&p->jobs[index%p->slots];
    pthread_mutex_lock(&p->mutex);
    while (job->index==index && job->state!=3)
        pthread_cond_wait(&p->complete,&p->mutex);
    int result=(job->index==index && job->state==3) ? job->result : -1;
    pthread_mutex_unlock(&p->mutex);
    return result;
}
void hdma_pool_release(HdmaWorkerPool *p,uint64_t index)
{
    HdmaFrameJob *job=&p->jobs[index%p->slots];
    pthread_mutex_lock(&p->mutex);
    if (job->index==index && job->state==3)job->state=0;
    pthread_mutex_unlock(&p->mutex);
}
void hdma_pool_destroy(HdmaWorkerPool *p)
{
    if (!p) return;
    if (p->initialized) {
        pthread_mutex_lock(&p->mutex);
        p->stopping=1;
        pthread_cond_broadcast(&p->ready);
        pthread_mutex_unlock(&p->mutex);
        for (unsigned i=0;i<p->started;i++)pthread_join(p->workers[i],NULL);
        pthread_cond_destroy(&p->complete);
        pthread_cond_destroy(&p->ready);
        pthread_mutex_destroy(&p->mutex);
    }
    free(p->workers);free(p->jobs);memset(p,0,sizeof(*p));
}
