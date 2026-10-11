/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later. */
#ifndef HDMA_SNDFILE_INPUT_H
#define HDMA_SNDFILE_INPUT_H
#include <stdint.h>
typedef struct PcmFileInput PcmFileInput;
int pcm_file_input_open(PcmFileInput **out, const char *filename, const char *channel_layout);
int pcm_file_input_rate(const PcmFileInput*source);
int pcm_file_input_channels(const PcmFileInput*source);
int pcm_file_input_depth(const PcmFileInput*source);
int pcm_file_input_layout21(const PcmFileInput*source);
int64_t pcm_file_input_frames(const PcmFileInput*source);
/* Returns 0..512 frames; a short read may only occur at EOF. */
int pcm_file_input_read(PcmFileInput*source,int32_t *interleaved,int frames);
const char *pcm_file_input_error(const PcmFileInput*source);
void pcm_file_input_close(PcmFileInput*source);
#endif
