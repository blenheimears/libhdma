/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Bounded, frame-ordered DCA + XLL XLL worker queue.
 */
#ifndef HDMA_XLL_WORKERS_H
#define HDMA_XLL_WORKERS_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define HDMA_JOB_CAPACITY 65536U
#define HDMA_JOB_SAMPLES (1024U * 8U)
typedef struct {
    int32_t pcm[HDMA_JOB_SAMPLES];
    int32_t core_pcm[HDMA_JOB_SAMPLES];
    uint8_t packet[HDMA_JOB_CAPACITY];
    uint64_t index;
    int core_bytes, got, packet_bytes, used_core, result;
    int force_full;
    uint64_t source_frames_done;
    int state; /* 0 free, 1 ready, 2 processing, 3 complete */
} HdmaFrameJob;
typedef struct {
    HdmaFrameJob *jobs;
    pthread_t *workers;
    unsigned slots, worker_count, started;
    int channels, rate, depth, mode, lossless, ultra, frame_samples, side_policy, layout21, hierarchy_mode;
    int max_prediction_order, max_fixed_prediction_order, parameter_reuse;
    int decorrelation_mode, max_segments, force_raw, stopping;
    int initialized;
    pthread_mutex_t mutex;
    pthread_cond_t ready, complete;
} HdmaWorkerPool;
/* thread_count == 1 executes lossless compression inline, without a worker. */
int hdma_pool_init(HdmaWorkerPool *pool, unsigned thread_count,
                   int channels, int rate, int depth, int mode, int lossless,int ultra,
                   int frame_samples, int side_policy,int layout21,int hierarchy_mode,int max_prediction_order,
                   int max_fixed_prediction_order,int parameter_reuse,
                   int decorrelation_mode,int max_segments,int force_raw);
HdmaFrameJob *hdma_pool_slot(HdmaWorkerPool *pool, uint64_t index);
int hdma_pool_submit(HdmaWorkerPool *pool, uint64_t index);
int hdma_pool_wait(HdmaWorkerPool *pool, uint64_t index);
void hdma_pool_release(HdmaWorkerPool *pool, uint64_t index);
void hdma_pool_destroy(HdmaWorkerPool *pool);
#endif
