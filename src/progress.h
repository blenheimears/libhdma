/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef LIBHDMA_PROGRESS_H
#define LIBHDMA_PROGRESS_H
#include <stdint.h>
#include <time.h>

typedef struct HdmaProgress {
    uint64_t source_samples, total, done, output_bytes;
    int rate, channels, depth, tty, enabled, active, last_percent, ratio_container,frame_samples;
    struct timespec started, last_update;
} HdmaProgress;

/* Only the ordered output thread updates progress; workers never write stderr.
 * Default ratio = raw codec packet bytes / processed uncompressed PCM bytes.
 * Optional --ratio-container includes Matroska overhead and final index.
 * PCM byte count uses the input's actual 16-/24-bit sample resolution. */
void hdma_progress_begin(HdmaProgress *p, uint64_t source_samples, int rate,
                         int channels, int depth, int enabled,int ratio_container,int frame_samples);
void hdma_progress_advance(HdmaProgress *p, uint64_t completed_frames,
                           uint64_t codec_bytes,uint64_t container_bytes);
/* Final container bytes include Matroska Cues and finalized headers. */
void hdma_progress_finish(HdmaProgress *p, uint64_t final_codec_bytes,
                          uint64_t final_container_bytes);
/* Remove a partially displayed terminal line before printing an error. */
void hdma_progress_abort(HdmaProgress *p);
#endif
