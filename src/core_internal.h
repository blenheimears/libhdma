/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later.
 * Compatibility-free internal DCA structures: no FFmpeg headers or libraries. */
#ifndef DCA_CORE_INTERNAL_H
#define DCA_CORE_INTERNAL_H
#include "libhdma.h"
#include "core_params.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif
#define DCA_ALIGN(v, a) (((v) + (a) - 1) & ~((a) - 1))
#define DCA_ABS(x) ((x) == INT32_MIN ? INT32_MAX : ((x) < 0 ? -(x) : (x)))
#define DCA_SWAP(type,a,b) do { type swp = (a); (a) = (b); (b) = swp; } while(0)
#define DCA_ERROR(x) (-(x))
#define DCA_LOG_ERROR 1
#define DCA_LOCAL_ALIGNED_32(type, name, size) type name size
#define dca_exp10(x) pow(10.0, (x))
typedef struct { int nb_channels; int stereo_lfe; } DCAChannelLayout;
typedef struct {
    void *priv_data;
    DCAChannelLayout ch_layout;
    int sample_rate;
    int64_t bit_rate;
    int frame_size;
} DCACodecContext;
typedef struct { const uint8_t *data[1]; } DCAFrame;
typedef struct { uint8_t *data; int size; } DCAPacket;
static inline int dca_get_encode_buffer(DCACodecContext *ctx, DCAPacket *pkt, int size, int flags) {
    (void)ctx; (void)flags;
    if (pkt->size < size) return DCA_ERROR(ENOMEM);
    pkt->size = size;
    return 0;
}
static inline void dca_log(void *ctx, int level, const char *fmt, ...) {
    (void)ctx; (void)level;
    va_list args; va_start(args,fmt); vfprintf(stderr,fmt,args); va_end(args);
}
typedef struct {
    uint8_t *start;
    uint8_t *buf;
    size_t capacity;
    size_t bits;
    int overflow;
} DcaBitWriter;
static inline void dca_bits_init(DcaBitWriter *b, uint8_t *dest, int bytes) {
    b->start = dest; b->buf=dest; b->capacity=(size_t)bytes;
    b->bits=0; b->overflow=0; memset(dest,0,(size_t)bytes);
}
static inline void dca_bits_write(DcaBitWriter *b, int n, uint32_t val) {
    if (n<0 || n>32 || (size_t)n > b->capacity*8 - b->bits) { b->overflow=1; return; }
    for(int i=n-1;i>=0;i--) {
        size_t bit=b->bits++;
        b->start[bit>>3] |= (uint8_t)(((val>>i)&1u)<<(7-(bit&7)));
    }
}
static inline void dca_bits_signed(DcaBitWriter *b, int n, int32_t val) {
    dca_bits_write(b,n,(uint32_t)val);
}
static inline void dca_bits_flush(DcaBitWriter *b) { (void)b; }
static inline uint8_t *dca_bits_ptr(DcaBitWriter *b) { return b->start+((b->bits+7)/8); }
static inline int dca_bits_bytes_left(DcaBitWriter *b, int round) {
    (void)round; return (int)(b->capacity-(b->bits+7)/8);
}
#define DCA_CODE_BOOKS 10
#define DCA_BITALLOC_12_COUNT 5
#define DCA_NUM_BITALLOC_CODES (1 * 3 + 3 * (5+7+9+13) + 7 * (17+25+33+65+129))
#define DCA_ADPCM_COEFFS 4
extern const uint32_t dca_bit_rates[32];
extern const uint8_t dca_quant_index_sel_nbits[DCA_CODE_BOOKS];
extern const uint8_t dca_quant_index_group_size[DCA_CODE_BOOKS];
extern const uint32_t dca_quant_levels[32];
extern const uint32_t dca_lossy_quant[32];
extern const float dca_fir_32bands_perfect[512];
extern const float dca_fir_32bands_nonperfect[512];
extern const float dca_lfe_fir_64[256];
extern const uint8_t dca_bitalloc_sizes[DCA_CODE_BOOKS];
extern const int8_t dca_bitalloc_offsets[DCA_CODE_BOOKS];
extern const uint8_t dca_vlc_src_tables[][2];
void standalone_mdct(int32_t dst[256], const int32_t src[512]);
#endif
