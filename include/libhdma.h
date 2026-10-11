/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LIBHDMA_H
#define LIBHDMA_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DcaCore DcaCore;
/* Baseline PCM: 512 interleaved signed 32-bit samples per channel, left-aligned.
 * Matching 1024-sample API is declared below; do not mix frame-size variants. */
int dca_core_create(DcaCore **out, int channels, int sample_rate, int bitrate);
/* Explicit 2.1: 3 interleaved speakers FL, FR, LFE (never treat LFE as center). */
int dca_core_create_layout(DcaCore **out, int channels, int rate, int bitrate, int stereo_lfe);
/* Enable optional DCA subband ADPCM before the first frame. Default off.
 * Prediction changes the compatibility core, not the lossless guarantee. */
int dca_core_set_adpcm(DcaCore *codec,int enabled);
int dca_core_set_source_depth(DcaCore *codec,int depth);
int dca_core_adpcm_bands(const DcaCore *codec); /* active bands in latest frame */
int dca_core_frame_bytes(const DcaCore *codec);
int dca_core_encode(DcaCore *codec, const int32_t *pcm, uint8_t *out, size_t capacity);
/* Reconstruct decoder-exact 24-bit core PCM from the preceding encoded frame.
 * Output is interleaved in original input order; at least 512*channels ints. */
int dca_core_reconstruct(DcaCore *codec, int32_t *out, size_t samples);
void dca_core_destroy(DcaCore *codec);
/* Matching API for an independently decodable 1024-sample DCA core frame.
 * A core instance must be used only with functions of its frame size. */
int dca_core_create_1024(DcaCore **out,int channels,int rate,int bitrate);
int dca_core_create_layout_1024(DcaCore **out,int channels,int rate,int bitrate,int stereo_lfe);
int dca_core_set_adpcm_1024(DcaCore *codec,int enabled);
int dca_core_set_source_depth_1024(DcaCore *codec,int depth);
int dca_core_adpcm_bands_1024(const DcaCore *codec);
int dca_core_frame_bytes_1024(const DcaCore *codec);
int dca_core_encode_1024(DcaCore *codec,const int32_t *pcm,uint8_t *dst,size_t capacity);
int dca_core_reconstruct_1024(DcaCore *codec,int32_t *dst,size_t samples);
void dca_core_destroy_1024(DcaCore *codec);
/* Full-sample XLL extension in one EXSS frame, no external FFmpeg linkage.
 * Experimental: byte-aligned 512-sample frame; pcm is left-aligned PCM16/24.
 * The caller appends the returned bytes after the aligned DCA core frame. */
int hdma_xll_encode(const int32_t *pcm, int channels, int sample_rate, int depth,
                    uint8_t *output, size_t capacity);
/* Explicit stereo + LFE speaker mapping; full-PCM XLL avoids false center. */
int hdma_xll_encode_21(const int32_t *pcm, int rate, int depth, uint8_t *output, size_t capacity);
int hdma_xll_encode_21_1024(const int32_t *pcm, int rate, int depth, uint8_t *output, size_t capacity);
/* Encoder-side core sample prediction; core_pcm is 24-bit fixed point.
 * For 7.x, the caller MUST map core surrounds into source side-channel
 * positions and leave source rear positions empty. The two rear speakers
 * are always full PCM because they have no core speaker counterpart.
 * AUTO compares full PCM, eligible all-core and mixed per-channel reference
 * masks. It emits the smallest complete EXSS candidate. FULL forces all
 * channels to independent PCM, CORE forces all AVAILABLE core speakers to
 * residuals, leaving unavailable 7.x rears full PCM; returns -2 if any
 * available core residual exceeds the signed input depth.
 * used_core receives the NUMBER of core-referenced channels (0..channels).
 * All policies preserve exact reconstruction. */
#define HDMA_XLL_REF_AUTO 0
#define HDMA_XLL_REF_FULL 1
#define HDMA_XLL_REF_CORE 2
int hdma_xll_encode_core_mode(const int32_t *pcm,const int32_t *core_pcm,
                              int channels,int rate,int depth,
                              uint8_t *output,size_t capacity,int mode,int *used_core);
/* Set ultra=0 for the fast default (no MSB/LSB split or exhaustive masks).
 * Set ultra=1 to search these additional, expensive options. */
int hdma_xll_encode_core_mode_options(const int32_t *pcm,const int32_t *core_pcm,
                                     int channels,int rate,int depth,
                                     uint8_t *output,size_t capacity,int mode,
                                     int *used_core,int ultra);
/* 1024-sample XLL variant must be paired with a 1024-sample DCA core. */
int hdma_xll_encode_1024(const int32_t *pcm,int channels,int rate,int depth,
                          uint8_t *output,size_t capacity);
int hdma_xll_encode_core_mode_options_1024(const int32_t *pcm,const int32_t *core_pcm,
                              int channels,int rate,int depth,uint8_t *output,
                              size_t capacity,int mode,int *used_core,int ultra);
int hdma_xll_encode_core(const int32_t *pcm, const int32_t *core_pcm,
                         int channels,int rate,int depth,
                         uint8_t *output,size_t capacity);
/* Attempt reference-style hierarchical XLL where supported: a primary
 * 5.0/5.1 set tied to the compatibility core plus a secondary two-channel
 * rear set with an embedded reversible downmix matrix. Unsupported
 * combinations transparently retain the proven single-set representation. */
#define HDMA_XLL_SIDE_BOTH  0
#define HDMA_XLL_SIDE_FRONT 1
#define HDMA_XLL_SIDE_REAR  2
#define HDMA_XLL_SIDE_MUTE  3
int hdma_xll_encode_hierarchical_7x_options(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy);
int hdma_xll_encode_hierarchical_7x_options_1024(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy);

#ifdef __cplusplus
}
#endif
#endif
