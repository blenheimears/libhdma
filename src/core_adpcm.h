/* Core ADPCM, adapted from FFmpeg 9.0.2 dcaadpcm.c/h, LGPL-2.1-or-later.
 * Copyright (C) 2017 Daniil Cherednik
 * Copyright (C) 2026 the contributors to the libhdma project.
 * Original license text: COPYING.LGPL-2.1. */
#ifndef HDMA_CORE_ADPCM_H
#define HDMA_CORE_ADPCM_H
#include <stdint.h>
#define HDMA_ADPCM_VECTORS 4096
extern const int16_t hdma_adpcm_vq[HDMA_ADPCM_VECTORS][4];
int hdma_adpcm_select(const int32_t *samples_with_history,int len,int32_t *diff);
int32_t hdma_adpcm_predict(int id,const int32_t history[4]);
int32_t hdma_adpcm_reconstruct(int id,int32_t residual,int32_t history[4]);
#endif
