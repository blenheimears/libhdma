/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later. */
#ifndef HDMA_RAW_PCM_INPUT_H
#define HDMA_RAW_PCM_INPUT_H
#include <stdint.h>
typedef struct RawPcmInput RawPcmInput;
/* Interleaved signed little-endian PCM16/PCM24; "-" reads stdin.
 * All format metadata is supplied explicitly, never guessed from raw bytes. */
int raw_pcm_open(RawPcmInput **out,const char *path,int rate,int channels,int depth,const char *layout);
int raw_pcm_rate(const RawPcmInput *in);
int raw_pcm_channels(const RawPcmInput *in);
int raw_pcm_depth(const RawPcmInput *in);
int raw_pcm_layout21(const RawPcmInput *in);
int64_t raw_pcm_frames(const RawPcmInput *in); /* -1 for an unknown-length pipe */
int raw_pcm_read(RawPcmInput *in,int32_t *dest,int frames);
const char *raw_pcm_error(const RawPcmInput *in);
void raw_pcm_close(RawPcmInput *in);
#endif
