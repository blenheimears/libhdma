/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Project-owned bitstream writer; format interpreted from the independently
 * developed FFmpeg 9.0.2 XLL and EXSS decoders (foo86 et al., LGPL).
 */
#ifndef LIBHDMA_XLL_H
#define LIBHDMA_XLL_H
#include <stddef.h>
#include <stdint.h>
#include "libhdma.h"
/* Set the maximum adaptive lattice-predictor order on the calling thread,
 * independently for 512- and 1024-sample XLL implementations. Range 0..15;
 * defaults to 10. Zero disables adaptive prediction; reference-compatible
 * fixed order 0 remains available.
 * Call before encoding a frame; changes to one worker do not affect another.
 * Returns -1 for out-of-range values without changing the current cap. */
int hdma_xll_set_max_prediction_order(int maximum);
int hdma_xll_set_max_prediction_order_1024(int maximum);
/* Decoder-compatibility controls. The reference-compatible encoder permits
 * fixed predictor order 0 only; hdma_xll_set_max_fixed_prediction_order()
 * therefore accepts only 0. Parameter reuse remains enabled, decorrelation is
 * enabled by default, and up to 16 segments may be selected. Each setting is
 * thread-local like the adaptive prediction cap. */
int hdma_xll_set_max_fixed_prediction_order(int maximum);
int hdma_xll_set_max_fixed_prediction_order_1024(int maximum);
int hdma_xll_set_parameter_reuse(int enabled);
int hdma_xll_set_parameter_reuse_1024(int enabled);
#define HDMA_XLL_DECOR_AUTO  (-1)
#define HDMA_XLL_DECOR_OFF    0
#define HDMA_XLL_DECOR_ON     1
int hdma_xll_set_decorrelation_mode(int mode);
int hdma_xll_set_decorrelation_mode_1024(int mode);
int hdma_xll_set_max_segments(int maximum);
int hdma_xll_set_max_segments_1024(int maximum);
/* Diagnostic compatibility control: force the uncompressed linear XLL
 * fallback instead of predictor/Rice/hybrid coding. Thread-local. */
int hdma_xll_set_force_raw(int enabled);
int hdma_xll_set_force_raw_1024(int enabled);
/* Builds a single EXSS frame with one lossless XLL component.
 * PCM is 512 interleaved left-aligned signed 32-bit integer input.
 * Output capacity must be at least 16384 bytes.
 * Returns EXSS length, or a negative value on failure. */
int hdma_xll_encode(const int32_t *pcm, int channels, int rate, int depth,
                    uint8_t *output, size_t capacity);
/* Lossless core-referenced coding. core_pcm is 512*channels interleaved,
 * signed decoded DCA core samples in native 24-bit integer units, with 7.x
 * core surround speakers mapped into source SIDE positions; the extra rear
 * speakers must not be mistaken for core speakers. */
int hdma_xll_encode_core(const int32_t *pcm, const int32_t *core_pcm,
                         int channels,int rate,int depth,
                         uint8_t *output,size_t capacity);
#define HDMA_XLL_HIER_OFF 0
#define HDMA_XLL_HIER_ON 1
#define HDMA_XLL_HIER_AUTO 2
/* 7.1 topology policy. ON keeps the DCA/XLL receiver-oriented two-set
 * hierarchy (embedded 5.1 primary + Lsr/Rsr) for every eligible
 * 24-bit/512-sample 7.1 frame and is the hdmaenc frontend default. OFF keeps
 * the older one-set diagnostic form. AUTO preserves the legacy per-frame
 * size comparison for diagnostics only. */
int hdma_xll_encode_hierarchical_7x_policy_options(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy,int hierarchy_mode);
int hdma_xll_encode_hierarchical_7x_policy_options_1024(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy,int hierarchy_mode);
/* Backward-compatible library API: retain the historical AUTO behavior for
 * callers that explicitly use this function. The hdmaenc frontend uses the
 * policy entry point above and selects stable hierarchy for eligible 7.1. */
int hdma_xll_encode_hierarchical_7x_options(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy);
int hdma_xll_encode_hierarchical_7x_options_1024(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy);

#endif
