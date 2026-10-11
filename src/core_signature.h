/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Core-only diagnostic. NEVER use the synthetic samples for lossless XLL.
 */
#ifndef LIBHDMA_CORE_SIGNATURE_H
#define LIBHDMA_CORE_SIGNATURE_H
#include <stdint.h>
/* Generate exactly one core frame of left-justified signed PCM.
 * A repeating, syncopated 16-step musical motif with rests and harmonics is
 * recognizable as an intentional pattern rather than a steady buzz or noise.
 * The LFE is silent; no high-frequency content is sent to a subwoofer.
 */
int hdma_core_signature(int32_t *dst, int samples, int channels, int rate,
                        int depth, int stereo_lfe, uint64_t frame_index);
#endif
