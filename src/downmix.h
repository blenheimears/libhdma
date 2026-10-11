/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#ifndef LIBHDMA_DOWNMIX_H
#define LIBHDMA_DOWNMIX_H
#include <stdint.h>
/* Input: canonical 7.0 (FL FR FC BL BR SL SR) or 7.1
 * (FL FR FC LFE BL BR SL SR), 512 interleaved left-aligned PCM samples.
 * Output: 5.0 (FL FR FC BL BR) or 5.1 (FL FR FC LFE BL BR).
 * The input is not modified; the lossless extension must always use it.
 * Returns the core channel count or -1 for an invalid source layout. */
typedef enum {
    HDMA_SIDE_BOTH = 0,
    HDMA_SIDE_FRONT = 1,
    HDMA_SIDE_REAR = 2,
    HDMA_SIDE_MUTE = 3
} HdmaSidePolicy;

/* Default BOTH is the standard compatibility fold: FL/FR/FC/LFE are copied
 * unchanged and the corresponding side + rear surrounds are mixed only into
 * the two 5.x surround outputs, using the same conservative -6.125 dB/input
 * gain as the embedded XLL receiver matrix. FRONT/REAR/MUTE are retained as
 * explicit legacy diagnostic overrides. Lossless XLL input is never changed. */
int hdma_downmix_core_policy(const int32_t *original, int channels,
                             int32_t *core, HdmaSidePolicy policy);
int hdma_downmix_core(const int32_t *original, int channels, int32_t *core);
/* Build the primary-set/core representation used by 7.x compatibility logic.
 * PRIMARY receives native-width unscaled hierarchy samples (so side+rear may
 * legitimately use 25 bits for 24-bit input); CORE receives left-aligned PCM
 * for the lossy 5.0/5.1 encoder using the selected compatibility policy. */
int hdma_downmix_hierarchical_policy_n(const int32_t *original, int channels,
                                       int32_t *primary, int32_t *core,
                                       HdmaSidePolicy policy, int samples, int depth);
int hdma_downmix_hierarchical_policy(const int32_t *original, int channels,
                                     int32_t *primary, int32_t *core,
                                     HdmaSidePolicy policy, int depth);

#endif
