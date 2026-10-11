/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Independent C writer for a deliberately small DCA EXSS/XLL subset.
 * Format reference: FFmpeg 9.0.2's LGPL dca_exss.c and dca_xll.c;
 * neither file is bundled, compiled, linked or used at runtime.
 */
#include "xll.h"
#ifdef HDMA_LONG_FRAME
#define HDMA_XLL_SAMPLES 1024
#define HDMA_XLL_LOG2 10
#define hdma_xll_set_max_prediction_order hdma_xll_set_max_prediction_order_1024
#define hdma_xll_set_max_fixed_prediction_order hdma_xll_set_max_fixed_prediction_order_1024
#define hdma_xll_set_parameter_reuse hdma_xll_set_parameter_reuse_1024
#define hdma_xll_set_decorrelation_mode hdma_xll_set_decorrelation_mode_1024
#define hdma_xll_set_max_segments hdma_xll_set_max_segments_1024
#define hdma_xll_set_force_raw hdma_xll_set_force_raw_1024
#define hdma_xll_encode hdma_xll_encode_1024
#define hdma_xll_encode_21 hdma_xll_encode_21_1024
#define hdma_xll_encode_core_mode_options hdma_xll_encode_core_mode_options_1024
#define hdma_xll_encode_core_mode hdma_xll_encode_core_mode_1024
#define hdma_xll_encode_core hdma_xll_encode_core_1024
#define hdma_xll_encode_hierarchical_7x_options hdma_xll_encode_hierarchical_7x_options_1024
#define hdma_xll_encode_hierarchical_7x_policy_options hdma_xll_encode_hierarchical_7x_policy_options_1024
#else
#define HDMA_XLL_SAMPLES 512
#define HDMA_XLL_LOG2 9
#endif
#include "simd.h"
#include "downmix.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include "xll_refl_table.h"

typedef struct {
    uint8_t *data;
    size_t capacity, position;
    int error;
} Bits;

static void put(Bits *b, uint32_t value, unsigned width)
{
    if (width > 32 || b->error || b->position > b->capacity * 8 ||
        width > b->capacity * 8 - b->position) { b->error = 1; return; }
    for (unsigned i = width; i; i--) {
        size_t pos = b->position++;
        uint8_t mask = (uint8_t)(1U << (7 - (pos & 7)));
        if ((value >> (i - 1)) & 1U) b->data[pos >> 3] |= mask;
        else b->data[pos >> 3] &= (uint8_t)~mask;
    }
}
static void patch(Bits *b, size_t bit, unsigned width, uint32_t value)
{
    size_t resume = b->position;
    if (bit > resume || width > resume - bit) { b->error=1; return; }
    b->position = bit; put(b,value,width); b->position=resume;
}
static void align_byte(Bits *b) { if (b->position & 7) put(b,0,(unsigned)(8-(b->position&7))); }
static uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc=0xffff;
    for (size_t i=0; i<size; ++i) {
        crc^=(uint16_t)data[i]<<8;
        for (int j=0;j<8;j++) crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    return crc;
}
static void finish_crc(Bits *b, size_t start)
{
    align_byte(b);
    if (b->error || (start & 7) || start>b->position) { b->error=1; return; }
    uint16_t crc=crc16(b->data+(start>>3),(b->position-start)>>3);
    put(b,crc,16);
}
static int freq_code(int rate)
{
    static const int rates[16]={8000,16000,32000,64000,128000,22050,44100,88200,
        176400,352800,12000,24000,48000,96000,192000,384000};
    for (int i=0;i<16;i++) if (rate==rates[i]) return i;
    return -1;
}
static int reference_clock_code(int rate)
{
    int code=freq_code(rate);
    if(code<0) return -1;
    if(code<=4) return 0;  /* 8/16/32-kHz clock family */
    if(code<=9) return 1;  /* 22.05/44.1-kHz clock family */
    return 2;              /* 12/24/48-kHz clock family */
}
static unsigned speaker_mask_layout(int channels,int stereo_lfe)
{
    if(stereo_lfe)return channels==3 ? 0x26 : 0; /* FL, FR, LFE */
    switch (channels) {
    case 1: return 0x01; /* C */
    case 2: return 0x06; /* L, R */
    case 3: return 0x07; /* C, L, R */
    case 4: return 0x1e; /* L, R, Ls, Rs */
    case 5: return 0x1f; /* C, L, R, Ls, Rs */
    case 6: return 0x3f; /* C, L, R, Ls, Rs, LFE */
    case 7: return 0x19f; /* C, L, R, Ls, Rs, Lsr, Rsr */
    case 8: return 0x1bf; /* C, L, R, Ls, Rs, LFE, Lsr, Rsr */
    default: return 0;
    }
}
static unsigned pair_mask_layout(int channels,int stereo_lfe)
{
    if(stereo_lfe)return channels==3 ? 0x0a : 0; /* FL/FR pair and LFE */
    switch (channels) {
    case 1: return 0x01; /* C */
    case 2: return 0x02; /* L/R */
    case 3: return 0x03;
    case 4: return 0x06; /* L/R + surrounds */
    case 5: return 0x07;
    case 6: return 0x0f;
    case 7: return 0x47;
    case 8: return 0x4f;
    default: return 0;
    }
}
static int channel_index_layout(int channels, int channel,int stereo_lfe)
{
    /* XLL channels follow increasing DCA speaker-bit order, not WAV order.
     * WAV/FLAC input: L, R, C, LFE, SL, SR. */
    static const int order[8][8]={{0},{0,1},{2,0,1},{0,1,2,3},
                                  {2,0,1,3,4},{2,0,1,4,5,3},
                                  {2,0,1,5,6,3,4},
                                  {2,0,1,6,7,3,4,5}};
    if(stereo_lfe)return channels==3 ? channel : -1;
    return order[channels-1][channel];
}
static uint32_t zigzag(int32_t v)
{
    return ((uint32_t)v<<1) ^ (uint32_t)-(v<0);
}
static unsigned bits_required_u32(uint32_t value)
{
    unsigned bits=1;
    while(value>>bits) bits++;
    return bits;
}
/* XLL syntax can represent fixed predictor orders 0..3, but the supplied
 * reference encoder uses fixed order 0 exclusively and switches to adaptive
 * prediction when prediction is worthwhile. Some standalone disc decoders
 * audibly mis-decode libhdma streams that use fixed orders 1..3, particularly
 * in the hierarchical rear channel set. For interoperability, libhdma follows
 * the reference encoder and never selects fixed orders above 0.
 *
 * Adaptive predictor selection still uses actual lossless entropy costs.
 * Segment-local Rice widths remain independent. Do not use floating point to
 * select a predictor or to form residuals.
 */
/* width is the Rice parameter or signed-linear width.  hybrid_width is
 * independent of Rice k and is only used for isolated part-B samples. */
typedef struct {
    unsigned rice, width, hybrid, hybrid_width;
    uint64_t bits;
} Coding;
/* The XLL subheader carries a four-bit order (0 means fixed prediction).
 * FFmpeg's decoder accommodates all orders through 15. */
#define HDMA_ADAPT_MAX_ORDER 15
/* The encoder's independent 512- and 1024-sample entry points each have
 * thread-local configuration. A worker explicitly sets this before processing
 * every frame; direct library calls default to 10. This is a selection cap,
 * not a request to force all channels to use the specified order. */
static _Thread_local int hdma_max_prediction_order = 10;
static _Thread_local int hdma_max_fixed_prediction_order = 0;
static _Thread_local int hdma_parameter_reuse = 1;
static _Thread_local int hdma_decorrelation_mode = HDMA_XLL_DECOR_ON;
static _Thread_local int hdma_max_segments = 16;
static _Thread_local int hdma_force_raw = 0;
int hdma_xll_set_max_prediction_order(int maximum)
{
    if (maximum < 0 || maximum > HDMA_ADAPT_MAX_ORDER) return -1;
    hdma_max_prediction_order = maximum;
    return 0;
}
int hdma_xll_set_max_fixed_prediction_order(int maximum)
{
    /* The supplied reference encoder never emits fixed predictor orders 1..3.
     * Several hardware/software DCA/XLL decoders also mis-handle those legal
     * orders in hierarchical 7.1. Keep only the reference-compatible fixed
     * order 0; adaptive prediction remains available independently. */
    if (maximum != 0) return -1;
    hdma_max_fixed_prediction_order = 0;
    return 0;
}
int hdma_xll_set_parameter_reuse(int enabled)
{
    if (enabled != 0 && enabled != 1) return -1;
    hdma_parameter_reuse = enabled;
    return 0;
}
int hdma_xll_set_decorrelation_mode(int mode)
{
    if (mode < HDMA_XLL_DECOR_AUTO || mode > HDMA_XLL_DECOR_ON) return -1;
    hdma_decorrelation_mode = mode;
    return 0;
}
int hdma_xll_set_max_segments(int maximum)
{
    if (maximum != 2 && maximum != 4 && maximum != 8 && maximum != 16) return -1;
    hdma_max_segments = maximum;
    return 0;
}
int hdma_xll_set_force_raw(int enabled)
{
    if (enabled != 0 && enabled != 1) return -1;
    hdma_force_raw = enabled;
    return 0;
}

typedef struct {
    Coding coding[2], first;
    unsigned fixed_order, adapt_order;
    int refl[HDMA_ADAPT_MAX_ORDER];
    uint64_t bits;
} ChannelPlan;

static int32_t pcm_sample(const int32_t *pcm, int channels, int depth,
                          int channel, int sample, int raw)
{
    int32_t v = pcm[(size_t)sample * (size_t)channels + (size_t)channel];
    return raw ? v : depth == 16 ? v / 65536 : v / 256;
}

static void make_differences(int32_t data[HDMA_XLL_SAMPLES], unsigned order)
{
#ifdef HDMA_LONG_FRAME
    for (unsigned pass=0;pass<order;pass++)
        for (int i=HDMA_XLL_SAMPLES-1;i>0;i--)
            data[i]=(int32_t)((uint32_t)data[i]-(uint32_t)data[i-1]);
#else
    hdma_simd_differences(data, order);
#endif
}

/* Coding sizes are computed using the exact same lengths written by the bit
 * writer; no floating-point decision can select a worse entropy payload. */
static unsigned signed_width(uint32_t code)
{
    unsigned w = 0;
    while (code) { ++w; code >>= 1; }
    return w;
}

/* The hybrid XLL syntax stores the number of isolated samples and their
 * part-B-relative indexes using log2(segment samples) bits each, followed by
 * a mixed Rice/linear payload. Isolated samples must fit the independently
 * signalled signed-linear width. All costs include their full metadata. */
static Coding select_coding_range(const int32_t *samples, int count, int depth,
                                  int required_rice, unsigned segment_log2,
                                  int allow_hybrid)
{
    uint32_t codes[HDMA_XLL_SAMPLES];
    uint32_t combined = hdma_simd_zigzag(samples, codes, count);
    unsigned width = signed_width(combined);
    if (width == 1) width = 2;
    Coding best = {0, width, 0, 0,
        width <= (unsigned)depth && required_rice != 1
             ? (uint64_t)count * width : UINT64_MAX};
    if (required_rice == 0) return best;
    unsigned max_k = depth == 16 ? 15U : 31U;
    unsigned max_linear = depth == 16 ? 16U : 32U;
    for (unsigned k = 0; k <= max_k; ++k) {
        uint64_t rice_cost = hdma_simd_rice_quotients(codes,count,k) +
                             (uint64_t)count * (1U + k);
        if (rice_cost < best.bits)
            best = (Coding){1, k, 0, 0, rice_cost};
        if (!allow_hybrid || count < 2) continue;
        for (unsigned linear = 1; linear <= max_linear; ++linear) {
            uint64_t cost = segment_log2; /* hybrid-width header is costed once by grid planner */
            unsigned isolated = 0;
            for (int n = 0; n < count; ++n) {
                uint64_t rb = (uint64_t)(codes[n] >> k) + 1U + k;
                if (signed_width(codes[n]) <= linear &&
                    rb > (uint64_t)linear + segment_log2) {
                    cost += (uint64_t)linear + segment_log2;
                    ++isolated;
                } else cost += rb;
                if (cost >= best.bits) break;
            }
            if (isolated && isolated < (1U << segment_log2) &&
                cost < best.bits)
                best = (Coding){1, k, 1, linear, cost};
        }
    }
    return best;
}

static ChannelPlan plan_fixed(const int32_t source[HDMA_XLL_SAMPLES], int depth)
{
    ChannelPlan best = {0};
    best.bits = UINT64_MAX;
    int32_t tmp[HDMA_XLL_SAMPLES];
    memcpy(tmp, source, sizeof(tmp));
    for (unsigned order = 0; order <= (unsigned)hdma_max_fixed_prediction_order; ++order) {
        if (order) make_differences(tmp, 1);
        Coding a = select_coding_range(tmp, HDMA_XLL_SAMPLES/2, depth, -1, HDMA_XLL_LOG2-1, 0);
        Coding b = select_coding_range(tmp + HDMA_XLL_SAMPLES/2, HDMA_XLL_SAMPLES/2, depth, -1, HDMA_XLL_LOG2-1, 0);
        uint64_t bits = a.bits + b.bits;
        if (bits < best.bits) {
            best.fixed_order = order;
            best.coding[0] = a;
            best.coding[1] = b;
            best.bits = bits;
        }
    }
    return best;
}

/* Apply precisely the inverse of FFmpeg's XLL lattice predictor using
 * 64-bit intermediate arithmetic.  Decoder direct-form coefficients are
 * generated from quantized reflection indices, with Q16 rounding at each
 * lattice step.  The first order samples remain uncoded prediction seeds. */
static int make_adaptive(int32_t residual[HDMA_XLL_SAMPLES], const int32_t source[HDMA_XLL_SAMPLES],
                         int order, const int reflect[HDMA_ADAPT_MAX_ORDER])
{
    int direct[HDMA_ADAPT_MAX_ORDER] = {0};
    for (int j = 0; j < order; ++j) {
        int rc = reflect[j];
        for (int k = 0; k < (j + 1) / 2; ++k) {
            int a = direct[k], b = direct[j-k-1];
            int64_t left = (int64_t)a + (((int64_t)rc * b + 32768) >> 16);
            int64_t right = (int64_t)b + (((int64_t)rc * a + 32768) >> 16);
            /* Do not emit coefficients whose direct-form expansion would
             * overflow the decoder's signed 32-bit coefficient arithmetic. */
            if (left < INT_MIN || left > INT_MAX ||
                right < INT_MIN || right > INT_MAX) return 0;
            direct[k] = (int)left;
            direct[j-k-1] = (int)right;
        }
        direct[j] = rc;
    }
    memcpy(residual, source, sizeof(int32_t) * HDMA_XLL_SAMPLES);
    for (int n = order; n < HDMA_XLL_SAMPLES; ++n) {
        int64_t sum = 0;
        for (int j = 0; j < order; ++j)
            sum += (int64_t)source[n-order+j] * direct[order-j-1];
        int64_t prediction = (sum + 32768) >> 16;
        if (prediction > 8388607) prediction = 8388607;
        if (prediction < -8388608) prediction = -8388608;
        residual[n] = (int32_t)((uint32_t)source[n] + (uint32_t)prediction);
    }
    return 1;
}

static int nearest_coeff(double target);

/* Levinson-Durbin estimates reflection coefficients.  Estimates ONLY seed
 * candidates; the final selection measures the actual written XLL bits.
 * The negative sign agrees with the decoder's subtractive inverse filter. */
static void estimate_reflections(const int32_t source[HDMA_XLL_SAMPLES],
                                 int estimated[HDMA_ADAPT_MAX_ORDER])
{
    double corr[HDMA_ADAPT_MAX_ORDER + 1] = {0};
    for (int lag = 0; lag <= HDMA_ADAPT_MAX_ORDER; ++lag)
        for (int n = lag; n < HDMA_XLL_SAMPLES; ++n)
            corr[lag] += (double)source[n] * source[n-lag];
    double a[HDMA_ADAPT_MAX_ORDER] = {0};
    double error = corr[0];
    for (int order = 0; order < HDMA_ADAPT_MAX_ORDER; ++order) {
        double cross = corr[order+1];
        for (int j = 0; j < order; ++j)
            cross += a[j] * corr[order-j];
        double rc = error > corr[0] * 1e-16 && error > 1.0
                        ? -cross / error : 0.0;
        if (rc < -0.99998) rc = -0.99998;
        if (rc >  0.99998) rc =  0.99998;
        estimated[order] = nearest_coeff(rc);
        double next[HDMA_ADAPT_MAX_ORDER];
        memcpy(next, a, sizeof(next));
        for (int j = 0; j < order; ++j)
            next[j] = a[j] + rc * a[order-j-1];
        next[order] = rc;
        memcpy(a, next, sizeof(a));
        error *= 1.0-rc*rc;
    }
}

static int nearest_coeff(double target)
{
    int sign = target < 0 ? -1 : 1;
    double magnitude = fabs(target);
    if (magnitude > 0.99998) magnitude = 0.99998;
    int best = 0;
    double distance = magnitude * 65536;
    for (int i = 1; i < 128; ++i) {
        double d = fabs(magnitude * 65536 - hdma_xll_refl_coeff[i]);
        if (d < distance) { best = i; distance = d; }
    }
    return sign * best;
}

static int refl_value(int idx)
{
    return idx < 0 ? -(int)hdma_xll_refl_coeff[-idx] :
                      (int)hdma_xll_refl_coeff[idx];
}

static ChannelPlan select_channel(const int32_t source[HDMA_XLL_SAMPLES], int depth)
{
    ChannelPlan best = plan_fixed(source, depth);
    int seed[HDMA_ADAPT_MAX_ORDER];
    estimate_reflections(source, seed);
    int32_t residual[HDMA_XLL_SAMPLES];
    /* Evaluate all syntax-supported orders, including orders 9..15.
     * Also try individually perturbing each lattice coefficient by +/-1 to
     * compensate for coarse quantization of the spec's lookup table. */
    for (int order = 1; order <= hdma_max_prediction_order; ++order) {
        for (int variant = -1; variant < 2*order; ++variant) {
            int indices[HDMA_ADAPT_MAX_ORDER], reflect[HDMA_ADAPT_MAX_ORDER];
            memcpy(indices, seed, sizeof(indices));
            if (variant >= 0) {
                int pos = variant / 2;
                indices[pos] += variant % 2 ? 1 : -1;
                if (indices[pos] < -127 || indices[pos] > 127) continue;
            }
            for (int j=0;j<order;j++) reflect[j]=refl_value(indices[j]);
            if (!make_adaptive(residual,source,order,reflect)) continue;
            Coding a=select_coding_range(residual+order,HDMA_XLL_SAMPLES/2-order,depth,-1,HDMA_XLL_LOG2-1,0);
            Coding b=select_coding_range(residual+HDMA_XLL_SAMPLES/2,HDMA_XLL_SAMPLES/2,depth,-1,HDMA_XLL_LOG2-1,0);
            Coding first=select_coding_range(residual,order,depth,(int)a.rice,8,0);
            /* A high-order predictor can need more than source-depth bits for
             * its unpredicted seed samples. The XLL linear field cannot
             * represent that width: switch BOTH parts to Rice. */
            if (!a.rice && first.bits == UINT64_MAX) {
                a=select_coding_range(residual+order,HDMA_XLL_SAMPLES/2-order,depth,1,HDMA_XLL_LOG2-1,0);
                first=select_coding_range(residual,order,depth,1,8,0);
            }
            uint64_t bits=a.bits+b.bits+first.bits +
                          4U+(unsigned)(8*order);
            if (bits < best.bits) {
                best=(ChannelPlan){0};
                best.coding[0]=a;
                best.coding[1]=b;
                best.first=first;
                best.adapt_order=(unsigned)order;
                memcpy(best.refl, indices, sizeof(best.refl));
                best.bits=bits;
            }
        }
    }
    return best;
}

static void put_rice(Bits *b, uint32_t value, unsigned k)
{
    uint32_t unary = value >> k;
    /* FFmpeg's unary decoder counts zero bits until the terminating one. */
    for (uint32_t j = 0; j < unary; ++j) put(b, 0, 1);
    put(b, 1, 1);
    if (k) put(b, value & ((UINT32_C(1) << k) - 1), k);
}
/* The grid is a complete entropy-coding plan. Explicit parameters, reuse,
 * common-channel allocation, NAVI, and final byte alignment all contribute
 * to the comparison. Decisions are stored and written verbatim: the encoder
 * never estimates reuse and subsequently emits independent parameters. */
typedef struct {
    Coding coding[16][8];
    Coding part_a[8];
    unsigned char reuse[16], common[16];
    unsigned grid_log, segment_log2;
    int nsegments, samples_per_segment;
    uint64_t bits;
} GridPlan;

/* Cost of a fixed, already-signalled allocation on NEW audio data. Reuse is
 * legal only if every residual can be represented by the stored allocation.
 * Hybrid part-B isolation positions are derived afresh in each segment. */
static uint64_t coding_cost(const int32_t *samples, int count, Coding choice,
                            unsigned log_samples)
{
    uint32_t codes[HDMA_XLL_SAMPLES];
    uint32_t combined = hdma_simd_zigzag(samples, codes, count);
    if (!choice.rice) {
        if (signed_width(combined) > choice.width) return UINT64_MAX;
        return (uint64_t)count * choice.width;
    }
    if (!choice.hybrid) {
        return hdma_simd_rice_quotients(codes,count,choice.width) +
               (uint64_t)count * (choice.width + 1U);
    }
    uint64_t total=log_samples;
    unsigned isolated=0;
    for (int n=0;n<count;n++) {
        uint64_t rice_bits=(uint64_t)(codes[n]>>choice.width)+choice.width+1U;
        if (signed_width(codes[n])<=choice.hybrid_width &&
            rice_bits>(uint64_t)choice.hybrid_width+log_samples) {
            total+=(uint64_t)choice.hybrid_width+log_samples;
            ++isolated;
        } else total+=rice_bits;
    }
    return isolated < (1U<<log_samples) ? total : UINT64_MAX;
}

static unsigned allocation_bits(int depth) { return depth==16 ? 4U : 5U; }

/* The common allocation must have a SINGLE rice/linear flag, part-A width,
 * and part-B width for all channels. Hybrid is not legal for common params.
 * On the first segment, FFmpeg's common decoder uses the highest prediction
 * order as the part-A length for ALL channels: only offer this mode when the
 * adaptive seed lengths agree. */
static int common_allocation(const int32_t data[8][HDMA_XLL_SAMPLES],const int adaptive[8],
                             int ch,int depth,int seg,int count,unsigned log,
                             Coding *result,Coding *seed_result,uint64_t *bits)
{
    unsigned abits=allocation_bits(depth);
    int seed=seg==0 ? adaptive[0] : 0;
    if (seg==0) for(int c=1;c<ch;c++) if(adaptive[c]!=seed) return 0;
    uint32_t aggregate_a=0,aggregate_b=0;
    uint64_t linear_bits=0;
    int limit=depth==16 ? 15 : 31;
    for(int c=0;c<ch;c++) {
        uint32_t scratch[HDMA_XLL_SAMPLES];
        if(seed) aggregate_a|=hdma_simd_zigzag(data[c],scratch,seed);
        aggregate_b|=hdma_simd_zigzag(data[c]+seg*count+seed,scratch,count-seed);
    }
    unsigned wa=signed_width(aggregate_a),wb=signed_width(aggregate_b);
    if(wa==1) wa=2;
    if(wb==1) wb=2;
    uint64_t lowest=UINT64_MAX;
    Coding best={0}, first={0};
    if(wa<=(unsigned)depth && wb<=(unsigned)depth) {
        linear_bits=(uint64_t)ch*(seed*wa+(count-seed)*wb);
        lowest=linear_bits;
        best=(Coding){0,wb,0,0,0};
        first=(Coding){0,wa,0,0,0};
    }
    /* Rice A and B allocations are independent. Aggregate costs once per k
     * instead of re-encoding every channel for each (kA,kB) pair. */
    uint64_t best_a=UINT64_MAX,best_b=UINT64_MAX;
    unsigned best_ka=0,best_kb=0;
    for(unsigned k=0;k<=(unsigned)limit;k++) {
        uint64_t ca=0,cb=0;
        for(int c=0;c<ch;c++) {
            uint32_t codes[HDMA_XLL_SAMPLES];
            if(seed) {
                hdma_simd_zigzag(data[c],codes,seed);
                ca+=hdma_simd_rice_quotients(codes,seed,k)+
                    (uint64_t)seed*(k+1U);
            }
            hdma_simd_zigzag(data[c]+seg*count+seed,codes,count-seed);
            cb+=hdma_simd_rice_quotients(codes,count-seed,k)+
                (uint64_t)(count-seed)*(k+1U);
        }
        if(ca<best_a) { best_a=ca;best_ka=k; }
        if(cb<best_b) { best_b=cb;best_kb=k; }
    }
    if(best_a+best_b<lowest) {
        lowest=best_a+best_b;
        best=(Coding){1,best_kb,0,0,0};
        first=(Coding){1,best_ka,0,0,0};
    }
    if(lowest==UINT64_MAX) return 0;
    uint64_t metadata=1U+1U+abits+(seg==0 ? abits : 0U);
    *result=best; *seed_result=first; *bits=lowest+metadata;
    (void)log;
    return 1;
}

static int build_grid(GridPlan *out,const int32_t data[8][HDMA_XLL_SAMPLES],
                      const int adaptive[8],int ch,int depth,unsigned lg)
{
    memset(out,0,sizeof(*out));
    out->grid_log=lg;
    out->segment_log2=HDMA_XLL_LOG2-lg;
    out->nsegments=1<<(int)lg;
    out->samples_per_segment=HDMA_XLL_SAMPLES>>(int)lg;
    unsigned abits=allocation_bits(depth);
    uint64_t total=(uint64_t)(((out->nsegments*14+7)/8)+2)*8;
    for(int seg=0;seg<out->nsegments;seg++) {
        int count=out->samples_per_segment;
        unsigned log=out->segment_log2;
        Coding distinct[8]={{0}},first[8]={{0}};
        uint64_t distinct_bits=1U; /* common=0 */
        int valid=1;
        for(int c=0;c<ch;c++) {
            int seed=seg==0 ? adaptive[c] : 0;
            distinct[c]=select_coding_range(data[c]+seg*count+seed,
                                             count-seed,depth,-1,log,1);
            if(seed) {
                first[c]=select_coding_range(data[c],seed,depth,
                                                (int)distinct[c].rice,log,0);
                if(!distinct[c].rice && first[c].bits==UINT64_MAX) {
                    distinct[c]=select_coding_range(data[c]+seed,count-seed,
                                                       depth,1,log,1);
                    first[c]=select_coding_range(data[c],seed,depth,1,log,0);
                }
            }
            if(distinct[c].bits==UINT64_MAX || (seed && first[c].bits==UINT64_MAX)) {
                valid=0;break;
            }
            distinct_bits+=distinct[c].bits+1U+abits+
                         (distinct[c].rice ? 1U : 0U)+
                         (distinct[c].hybrid ? abits : 0U);
            if(!seg) distinct_bits+=abits+first[c].bits;
        }
        if(!valid) return -1;
        unsigned kind=0; /* 0 distinct, 1 common, 2 reuse */
        uint64_t best=distinct_bits;
        Coding shared={0}, shared_seed={0};
        uint64_t common_bits;
        if(common_allocation(data,adaptive,ch,depth,seg,count,log,
                             &shared,&shared_seed,&common_bits) &&
           common_bits<best) {best=common_bits;kind=1;}
        int can_reuse=hdma_parameter_reuse && seg>0;
        /* Reuse also retains the decoder's part-A sample count. Segment 0
         * can have adaptive-predictor seed samples, whereas later segments
         * cannot. Segment 1 must explicitly reset that state first. */
        if(seg==1) for(int c=0;c<ch;c++)
            if(adaptive[c]) can_reuse=0;
        if(can_reuse) {
            /* Reuse *the previous effective parameter set*, which may be
             * common or distinct. Decoder state survives through reuse. */
            uint64_t reuse_bits=0;
            for(int c=0;c<ch;c++) {
                Coding old=out->coding[seg-1][c];
                uint64_t bits=coding_cost(data[c]+seg*count,count,old,log);
                if(bits==UINT64_MAX || reuse_bits>UINT64_MAX-bits) {
                    reuse_bits=UINT64_MAX;break;
                }
                reuse_bits+=bits;
            }
            if(reuse_bits<best) {best=reuse_bits;kind=2;}
        }
        uint64_t payload=best+(seg ? 1U : 0U); /* reuse flag if seg>0 */
        total+=(payload+7U)&~UINT64_C(7);
        if(kind==2) {
            out->reuse[seg]=1;
            out->common[seg]=out->common[seg-1];
            memcpy(out->coding[seg],out->coding[seg-1],
                   (size_t)ch*sizeof(Coding));
        } else if(kind==1) {
            out->common[seg]=1;
            for(int c=0;c<ch;c++) {
                out->coding[seg][c]=shared;
                if(!seg) out->part_a[c]=shared_seed;
            }
        } else {
            for(int c=0;c<ch;c++) {
                out->coding[seg][c]=distinct[c];
                if(!seg) out->part_a[c]=first[c];
            }
        }
    }
    out->bits=total;
    return 0;
}

/* Predictor search uses the actual candidate grid, not the hard-coded
 * 2x256 segmentation used by the original 0.0.14 heuristic. The cheap first
 * pass excludes hybrid-only outlier searches; build_grid then evaluates the
 * complete Rice/hybrid/reuse/common result for each joint grid candidate. */
static uint64_t predictor_grid_cost(const int32_t transformed[HDMA_XLL_SAMPLES],int depth,
                                    int seed,unsigned lg)
{
    int count=HDMA_XLL_SAMPLES>>(int)lg,segments=1<<(int)lg;
    unsigned log=HDMA_XLL_LOG2-lg,abits=allocation_bits(depth);
    uint64_t total=4U+(seed ? (unsigned)seed*8U : 2U);
    Coding old={0};
    for(int seg=0;seg<segments;seg++) {
        int nseed=seg==0 ? seed : 0;
        Coding b=select_coding_range(transformed+seg*count+nseed,
                                       count-nseed,depth,-1,log,0);
        Coding a={0};
        if(nseed) {
            a=select_coding_range(transformed,nseed,depth,(int)b.rice,log,0);
            if(a.bits==UINT64_MAX && !b.rice) {
                b=select_coding_range(transformed+nseed,count-nseed,
                                         depth,1,log,0);
                a=select_coding_range(transformed,nseed,depth,1,log,0);
            }
        }
        if(b.bits==UINT64_MAX || (nseed && a.bits==UINT64_MAX)) return UINT64_MAX;
        uint64_t independent=b.bits+1U+abits+(b.rice?1U:0U);
        if(!seg) independent+=abits+a.bits;
        if(seg) {
            if(hdma_parameter_reuse) {
                uint64_t reuse=coding_cost(transformed+seg*count,count,old,log);
                if(reuse<independent) independent=reuse;
                else old=b;
            } else old=b;
        } else old=b;
        total+=independent;
    }
    return total;
}

static int project_plan(int32_t dest[HDMA_XLL_SAMPLES],const int32_t source[HDMA_XLL_SAMPLES],
                         const ChannelPlan *plan)
{
    if(plan->adapt_order) {
        int coeff[HDMA_ADAPT_MAX_ORDER];
        for(unsigned j=0;j<plan->adapt_order;j++) coeff[j]=refl_value(plan->refl[j]);
        if (!make_adaptive(dest,source,(int)plan->adapt_order,coeff)) return 0;
    } else {
        memcpy(dest,source,HDMA_XLL_SAMPLES*sizeof(int32_t));
        make_differences(dest,plan->fixed_order);
    }
    return 1;
}

typedef int32_t XllSampleRow[HDMA_XLL_SAMPLES];

static ChannelPlan select_channel_for_grid(const int32_t source[HDMA_XLL_SAMPLES],int depth,
                                            unsigned lg,const ChannelPlan *base,
                                            XllSampleRow *transformed)
{
    ChannelPlan best=*base;
    uint64_t score=UINT64_MAX;
    int32_t candidate_data[HDMA_XLL_SAMPLES];
    int seeds[HDMA_ADAPT_MAX_ORDER];
    estimate_reflections(source,seeds);
    /* Retain the locally-refined lattice result and examine the
     * reference-compatible fixed order 0 plus adaptive predictor orders. */
    for(int candidate=-1;candidate<4+hdma_max_prediction_order;candidate++) {
        ChannelPlan plan={0};
        if(candidate==-1) plan=*base;
        else if(candidate<4) {
            if(candidate>hdma_max_fixed_prediction_order) continue;
            plan.fixed_order=(unsigned)candidate;
        } else {
            plan.adapt_order=(unsigned)(candidate-4+1);
            memcpy(plan.refl,seeds,sizeof(plan.refl));
        }
        if (!project_plan(candidate_data,source,&plan)) continue;
        uint64_t bits=predictor_grid_cost(candidate_data,depth,
                                             (int)plan.adapt_order,lg);
        if(bits<score) {
            score=bits;best=plan;
            memcpy(*transformed,candidate_data,sizeof(candidate_data));
        }
    }
    return best;
}

/* The decoder allows a permutation of the original channels followed by
 * independent reversible transforms on adjacent pairs. Search arbitrary
 * source/destination speakers and BOTH directions instead of assuming that
 * only the conventional L/R and surround pairs can share information.
 *
 * Cross-correlation cheaply shortlists oriented pairs. Actual candidate
 * selection uses the existing lossless integer predictor/entropy estimator;
 * the subset DP then chooses disjoint pairs, with at most one unpaired channel.
 * This is a bounded search, not an exponential permutation enumeration. */
typedef struct {
    int coeff;
    uint64_t gain;
    ChannelPlan plan;
} PairChoice;

static void choose_channel_pairs(const int32_t original[8][HDMA_XLL_SAMPLES],
                                 const ChannelPlan baseline[8],
                                 int ch, int depth, int order[8],
                                 int coeffs[4], ChannelPlan deltas[4],
                                 uint64_t *savings, int *enabled_pairs)
{
    PairChoice edge[8][8] = {{{0}}};
    double energy[8] = {0}, cross[8][8] = {{0}}, priority[8][8] = {{0}};
    const int shortlist = ch >= 7 ? 12 : 8;
    for (int i=0;i<ch;i++) {
        for (int n=0;n<HDMA_XLL_SAMPLES;n++) {
            double x=(double)original[i][n];
            energy[i]+=x*x;
        }
        for (int j=0;j<i;j++) {
            for (int n=0;n<HDMA_XLL_SAMPLES;n++)
                cross[i][j]+=(double)original[i][n]*original[j][n];
            cross[j][i]=cross[i][j];
        }
    }
    int preferred[8]={0,1,2,3,4,5,6,7};
    if(ch==5) {
        const int p[5]={1,2,3,4,0};
        memcpy(preferred,p,sizeof(p));
    } else if(ch==6) {
        const int p[6]={1,2,4,5,0,3};
        memcpy(preferred,p,sizeof(p));
    }
    for(int i=0;i<ch;i++) for(int j=0;j<ch;j++) if(i!=j && energy[i]>0.0 && energy[j]>0.0) {
        double a=cross[i][j]/energy[i];
        if(a<-8.0) a=-8.0;
        if(a>7.875) a=7.875;
        double err=energy[j]-2.0*a*cross[i][j]+a*a*energy[i];
        if(err<0.0) err=0.0;
        priority[i][j]=log2((energy[j]+1.0)/(err+1.0));
    }
    for(int i=0;i<ch;i++) for(int j=0;j<ch;j++) if(i!=j) {
        int rank=0, is_preferred=0;
        for(int a=0;a<ch;a++)for(int d=0;d<ch;d++)if(a!=d &&
            (priority[a][d]>priority[i][j] ||
             (priority[a][d]==priority[i][j] && (a<i || (a==i && d<j)))))
            rank++;
        for(int pair=0;pair<ch/2;pair++)
            if(preferred[2*pair]==i && preferred[2*pair+1]==j)
                is_preferred=1;
        if(rank>=shortlist && !is_preferred) continue;
        int estimate=energy[i]>0.0 ? (int)nearbyint(8.0*cross[i][j]/energy[i]) : 0;
        if(estimate<-64) estimate=-64;
        if(estimate>63) estimate=63;
        int candidates[6]={8,estimate-2,estimate-1,estimate,estimate+1,estimate+2};
        uint64_t candidate_cost=UINT64_MAX;
        int best_coeff=0;
        int32_t delta[HDMA_XLL_SAMPLES], best_delta[HDMA_XLL_SAMPLES];
        const int32_t limit=(int32_t)(1U<<(depth-1));
        for(int k=0;k<6;k++) {
            int coeff=candidates[k], valid=1;
            if(!coeff || coeff < -64 || coeff >63) continue;
            for(int n=0;n<HDMA_XLL_SAMPLES;n++) {
                int64_t prediction=((int64_t)original[i][n]*coeff+4)>>3;
                int64_t value=(int64_t)original[j][n]-prediction;
                if(value < -limit || value >=limit) {valid=0;break;}
                delta[n]=(int32_t)value;
            }
            if(!valid) continue;
            ChannelPlan p=plan_fixed(delta,depth);
            if(p.bits<candidate_cost) {
                candidate_cost=p.bits;
                best_coeff=coeff;
                memcpy(best_delta,delta,sizeof(delta));
            }
        }
        if(!best_coeff) continue;
        ChannelPlan p=select_channel(best_delta,depth);
        if(p.bits+15U<baseline[j].bits) {
            edge[i][j].coeff=best_coeff;
            edge[i][j].plan=p;
            edge[i][j].gain=baseline[j].bits-p.bits-15U;
        }
    }

    /* Maximum-weight matching over at most eight channels. A pair without
     * decorrelation has zero weight, and a 7-channel layout leaves one speaker
     * unchanged. The 15-bit penalty includes coefficient + flag overhead. */
    uint64_t dp[256]={0};
    int prev[256], src[256], dst[256];
    for(int mask=0;mask<256;mask++) {prev[mask]=-1;src[mask]=dst[mask]=-1;}
    prev[0]=0;
    int all=(1<<ch)-1;
    for(int mask=0;mask<=all;mask++) if(prev[mask]>=0) {
        for(int i=0;i<ch;i++)if(!(mask&(1<<i)))
            for(int j=i+1;j<ch;j++)if(!(mask&(1<<j))) {
                int next=mask|(1<<i)|(1<<j);
                int a=i,b=j;
                if(edge[j][i].gain>edge[i][j].gain) {a=j;b=i;}
                uint64_t score=dp[mask]+edge[a][b].gain;
                if(prev[next]<0 || score>dp[next]) {
                    prev[next]=mask;dp[next]=score;src[next]=a;dst[next]=b;
                }
            }
    }
    int finish=all,leftover=-1;
    if(ch&1) {
        finish=-1;
        for(int c=0;c<ch;c++) {
            int mask=all^(1<<c);
            if(prev[mask]>=0 && (finish<0 || dp[mask]>dp[finish])) {
                finish=mask;leftover=c;
            }
        }
    }
    int pair=ch/2;
    while(finish && pair>0) {
        pair--;
        order[pair*2]=src[finish];
        order[pair*2+1]=dst[finish];
        coeffs[pair]=edge[src[finish]][dst[finish]].coeff;
        if(coeffs[pair]) {
            deltas[pair]=edge[src[finish]][dst[finish]].plan;
            (*enabled_pairs)++;
            *savings+=baseline[dst[finish]].bits-deltas[pair].bits-8U;
        }
        finish=prev[finish];
    }
    if(leftover>=0) order[ch-1]=leftover;
}

typedef struct {
    int forced_grid_log;          /* -1 selects automatically */
    unsigned ch_mask_nbits;       /* common-header speaker mask width */
    unsigned ch_mask;             /* this channel set's speaker mask */
    int primary_chset;
    int hier_chset;
    int dmix_coeffs_present;
    int dmix_embedded;
    int hier_ofs;
    /* Some hardware-oriented hierarchical streams keep the scalable-LSB
     * syntax enabled even when every channel carries zero split LSB bits.
     * This is syntax-only: the PCM payload remains unchanged. */
    int force_scalable_lsb;
    uint16_t dmix_scale_code[8];
    uint16_t dmix_coeff_code[8][8];
    int input_order[8];           /* set-channel order -> PCM input index */
} XllSetMeta;

static int meta_input_channel(const XllSetMeta *meta,int channels,int channel,int stereo_lfe)
{
    return meta ? meta->input_order[channel] : channel_index_layout(channels,channel,stereo_lfe);
}

static int make_xll_once(const int32_t *pcm,int ch,int rate,int depth,Bits *b,
                    int raw_pcm,unsigned full_mask,const unsigned lsb_width[8],
                    const XllSetMeta *meta,int stereo_lfe,
                    unsigned frame_size_nbits,unsigned seg_size_nbits,
                    size_t *largest_segment)
{
    /* All fixed channel workspaces below have exactly eight rows.  Public
     * entry points already reject unsupported layouts, but keep the invariant
     * local as well: older GCC object-size analysis cannot infer it through
     * speaker_mask_layout()/metadata validation and otherwise diagnoses
     * transformed[c] as a possibly short row under -Wstringop-overflow. */
    if(ch < 1 || ch > 8) return -1;
    int freq=freq_code(rate);
    if (freq<0 || (!meta && !speaker_mask_layout(ch,stereo_lfe)) || (meta && !meta->ch_mask)) return -1;
    Coding coding[16][8];
    int nsegments = 2, samples_per_segment = HDMA_XLL_SAMPLES/2;
    unsigned grid_log = 1, segment_log2 = HDMA_XLL_LOG2-1;
    int predict[8] = {0};
    int adaptive[8] = {0};
    int reflection[8][HDMA_ADAPT_MAX_ORDER] = {{0}};
    Coding part_a[8];
    int orig_order[8];
    int decor_pairs[4] = {0};
    int decor_enabled = 0;
    int32_t original[8][HDMA_XLL_SAMPLES];
    int32_t channel_data[8][HDMA_XLL_SAMPLES];
    ChannelPlan plans[8];
    unsigned raw_width[8] = {0};
    unsigned lsb_bits_per_segment=0;
    for (int c=0;c<ch;c++) {
        if (lsb_width[c]>15 || lsb_width[c]>=(unsigned)depth) return -1;
        lsb_bits_per_segment+=lsb_width[c];
    }
    int split=lsb_bits_per_segment!=0 || (meta && meta->force_scalable_lsb);
    for (int c = 0; c < ch; ++c) {
        orig_order[c] = c;
        int input_ch = meta_input_channel(meta,ch,c,stereo_lfe);
        for (int n = 0; n < HDMA_XLL_SAMPLES; ++n) {
            int32_t value=pcm_sample(pcm, ch, depth, input_ch, n, raw_pcm);
            /* The decoder predicts/decorrelates MSBs first and appends
             * unsigned LSBs afterwards. A signed floor division is essential
             * for negative PCM: low bits are preserved without loss. */
            int64_t factor=INT64_C(1)<<lsb_width[c];
            original[c][n]=(int32_t)(value>=0 ? value/(int32_t)factor :
                           -((-((int64_t)value)+factor-1)/factor));
        }
        /* Predictor/decorrelation and hierarchical downmix intermediates are
         * permitted to need more bits than the final PCM resolution.  In
         * particular, a 24-bit 7.1 primary surround can be (side + rear),
         * which is a lossless 25-bit value.  The allocation field is five
         * bits wide for 24-bit XLL and can signal up to 32 signed bits; do
         * not force the raw fallback back into the source width. */
        uint32_t raw_codes=0;
        for (int n=0;n<HDMA_XLL_SAMPLES;n++) raw_codes|=zigzag(original[c][n]);
        raw_width[c]=signed_width(raw_codes);
        if(raw_width[c]<2U) raw_width[c]=2U;
        if(raw_width[c]>(depth==16 ? 16U : 32U)) return -1;
        plans[c] = select_channel(original[c], depth);
    }
    /* The original speaker identifiers are retained in orig_order[], even
     * when the encoder searches new oriented pairings of those speakers. */
    int pair_order[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    ChannelPlan delta_plans[4];
    uint64_t decor_savings = 0;
    int selected_pairs = 0;
    unsigned order_bits = ch <= 2 ? 1 : ch <= 4 ? 2 : 3;
    choose_channel_pairs((const int32_t (*)[HDMA_XLL_SAMPLES])original, plans, ch, depth,
                             pair_order, decor_pairs, delta_plans,
                             &decor_savings, &selected_pairs);
    uint64_t decor_extra = 0;
    /* Preserve rate-distortion selection: syntactically permitted channel
     * decorrelation is not required merely because one sample uses it. */
    if (selected_pairs) {
        decor_extra = (uint64_t)ch * order_bits + (unsigned)(ch / 2) +
                      (unsigned)selected_pairs * 7;
    }
    if (hdma_decorrelation_mode == HDMA_XLL_DECOR_ON)
        decor_enabled = 1;
    else if (hdma_decorrelation_mode == HDMA_XLL_DECOR_AUTO && selected_pairs &&
             decor_savings > decor_extra + 16)
        decor_enabled = 1;
    if (!decor_enabled) {
        memset(decor_pairs, 0, sizeof(decor_pairs));
        decor_extra = 0;
    }
    /* Preserve the undecoded channel samples while considering candidate
     * predictors and entropy grids jointly. The old 256-sample predictor
     * search supplies a refined starting candidate, not a final decision. */
    ChannelPlan initial[8];
    int32_t prepared[8][HDMA_XLL_SAMPLES];
    for (int c = 0; c < ch; ++c) {
        int source = decor_enabled ? pair_order[c] : c;
        orig_order[c] = source;
        memcpy(prepared[c], original[source], sizeof(prepared[c]));
        initial[c] = plans[source];
        if (decor_enabled && (c & 1) && decor_pairs[c / 2]) {
            int first = pair_order[c - 1];
            for (int n = 0; n < HDMA_XLL_SAMPLES; ++n)
                prepared[c][n] = (int32_t)((uint32_t)original[source][n] -
                    (uint32_t)(int32_t)(((int64_t)original[first][n] *
                                          decor_pairs[c / 2] + 4) >> 3));
            initial[c] = delta_plans[c / 2];
        }
    }
    GridPlan selected={0};
    uint64_t best_total=UINT64_MAX;
    /* DCA XLL limits each segment to 256 samples at <= 48 kHz and 512
     * samples above 48 kHz. A legal grid must satisfy this independently
     * of prediction mode, reference choice, and channel-set hierarchy. */
    unsigned max_segment_log = rate <= 48000 ? 8U : 9U;
    unsigned minimum_grid_log = HDMA_XLL_LOG2 > max_segment_log ?
                                HDMA_XLL_LOG2 - max_segment_log : 0U;
    unsigned grid_first = meta && meta->forced_grid_log >= 0 ?
                          (unsigned)meta->forced_grid_log : minimum_grid_log;
    if (grid_first < minimum_grid_log || grid_first > 4U) return -1;
    unsigned configured_grid_last = hdma_max_segments==2 ? 1U :
                                    hdma_max_segments==4 ? 2U :
                                    hdma_max_segments==8 ? 3U : 4U;
    unsigned grid_last = meta && meta->forced_grid_log >= 0 ? grid_first : configured_grid_last;
    if (grid_first > configured_grid_last) return -1;
    for(unsigned lg=grid_first;lg<=grid_last;lg++) {
        XllSampleRow transformed[8];
        ChannelPlan candidate[8];
        int candidate_adaptive[8]={0};
        uint64_t sub_header_bits=10U+4U+(unsigned)ch+5U+5U+4U+
                                 2U+2U+1U+1U+1U+1U+
                                 (ch>6?9U:6U)+1U+(unsigned)ch*4U;
        if(decor_enabled) sub_header_bits+=decor_extra;
        for(int c=0;c<ch && c<8;c++) {
            candidate[c]=select_channel_for_grid(prepared[c],depth,lg,
                                                   &initial[c],&transformed[c]);
            candidate_adaptive[c]=(int)candidate[c].adapt_order;
            sub_header_bits+=candidate[c].adapt_order ?
                            candidate[c].adapt_order*8U : 2U;
        }
        GridPlan proposed;
        if(build_grid(&proposed,(const int32_t (*)[HDMA_XLL_SAMPLES])transformed,
                      candidate_adaptive,ch,depth,lg)) continue;
        /* The variable portion of the subheader, including CRC and byte
         * alignment, is compared alongside exact segment and NAVI bits. */
        uint64_t actual=proposed.bits+((sub_header_bits+7U)&~UINT64_C(7))+16U;
        if(actual<best_total) {
            best_total=actual;
            selected=proposed;
            for(int c=0;c<ch;c++) {
                predict[c]=(int)candidate[c].fixed_order;
                adaptive[c]=(int)candidate[c].adapt_order;
                memcpy(reflection[c],candidate[c].refl,sizeof(reflection[c]));
                memcpy(channel_data[c],transformed[c],sizeof(channel_data[c]));
            }
        }
    }
    if(best_total!=UINT64_MAX) {
        nsegments=selected.nsegments;
        samples_per_segment=selected.samples_per_segment;
        grid_log=selected.grid_log;
        segment_log2=selected.segment_log2;
        memcpy(coding,selected.coding,sizeof(coding));
        memcpy(part_a,selected.part_a,sizeof(part_a));
    }
    unsigned allocbits=allocation_bits(depth);
    /* Compare complete XLL sizes for the selected joint search and the
     * no-prediction, minimum-legal-segment raw fallback. Include the exact variable
     * subheader, CRC, NAVI, allocations, and per-segment byte alignment. */
    uint64_t raw_header=10U+4U+(unsigned)ch+5U+5U+4U+2U+2U+
                        1U+1U+1U+1U+(ch>6?9U:6U)+1U+
                        (unsigned)ch*4U+(unsigned)ch*2U;
    /* The raw fallback must meet the same sampling-rate-dependent segment
     * ceiling as compressed grids (512-frame @48 kHz => two segments). */
    unsigned raw_nsegments=1U<<minimum_grid_log;
    /* Raw mode uses separate per-channel allocations so a reversible
     * transform can carry a 25..32-bit intermediate without pretending the
     * output PCM itself has that resolution. */
    uint64_t raw_parameter_bits=1U+(uint64_t)ch+2U*(uint64_t)ch*allocbits;
    if(raw_nsegments>1U) raw_parameter_bits+=(raw_nsegments-1U)*
        (hdma_parameter_reuse ? 1U : 2U+(uint64_t)ch+(uint64_t)ch*allocbits);
    uint64_t raw_payload=raw_parameter_bits;
    for(int c=0;c<ch;c++)
        raw_payload+=(uint64_t)HDMA_XLL_SAMPLES*raw_width[c];
    uint64_t raw_total=((raw_header+7U)&~UINT64_C(7))+16U+
                       ((raw_nsegments*14U+7U)/8U+2U)*8U+
                       ((raw_payload+7U)&~UINT64_C(7));
    int compressed=!hdma_force_raw && best_total!=UINT64_MAX && best_total<raw_total;
    if(!compressed) {
        nsegments=(int)raw_nsegments;
        samples_per_segment=HDMA_XLL_SAMPLES/nsegments;
        grid_log=minimum_grid_log;
        segment_log2=HDMA_XLL_LOG2-grid_log;
        decor_enabled=hdma_decorrelation_mode==HDMA_XLL_DECOR_ON;
        memset(decor_pairs,0,sizeof(decor_pairs));
        for(int c=0;c<ch;c++) {
            predict[c]=0; adaptive[c]=0; orig_order[c]=c;
            memcpy(channel_data[c],original[c],sizeof(channel_data[c]));
            for(int seg=0;seg<nsegments;seg++)
                coding[seg][c]=(Coding){0,raw_width[c],0,0,0};
        }
    }
    /* Exact selected entropy grid; all choices still encode HDMA_XLL_SAMPLES samples. */
    size_t start=b->position;
    put(b,0x41a29547U,32);
    put(b,0,4); /* XLL format version 1 */
    size_t common_size=b->position; put(b,0,8); /* whole common header bytes -1 */
    if(frame_size_nbits<1U || frame_size_nbits>20U ||
       seg_size_nbits<1U || seg_size_nbits>20U) return -1;
    put(b,frame_size_nbits-1U,5);
    size_t frame_size=b->position;put(b,0,frame_size_nbits);
    put(b,0,4); /* 1 channel set */
    put(b,grid_log,4); /* log2 frame segment count */
    put(b,segment_log2,4); /* log2 samples/segment */
    put(b,seg_size_nbits-1U,5);
    put(b,0,2); /* no per-band CRC */
    put(b,(uint32_t)split,1); /* optional scalable MSB/LSB section */
    put(b,(meta ? meta->ch_mask_nbits : (ch > 6 ? 9U : 6U))-1U,5);
    if (split) put(b,0,4); /* variable per-channel LSB widths */
    finish_crc(b,start+32);
    size_t common_crc_pos=b->position-16;
    if (b->error) return -1;
    patch(b,common_size,8,(uint32_t)((b->position-start)/8-1));
    patch(b,b->position-16,16,crc16(b->data+(start+32)/8,
                        (b->position-start-32)/8-2));

    size_t sub_start=b->position;
    size_t sub_size=b->position;put(b,0,10);
    put(b,(uint32_t)(ch-1),4);
    put(b,full_mask,(unsigned)ch); /* bit=1: full PCM; bit=0: core residual */
    put(b,(uint32_t)(depth-1),5); /* PCM bit resolution */
    put(b,(uint32_t)(depth-1),5); /* storage-unit width */
    put(b,(uint32_t)freq,4);
    put(b,0,2); /* no sample-rate modifier */
    put(b,0,2); /* first replacement set */
    put(b,(uint32_t)(meta ? meta->primary_chset : 1),1);
    put(b,(uint32_t)(meta ? meta->dmix_coeffs_present : 0),1);
    if (meta && meta->dmix_coeffs_present) {
        put(b,(uint32_t)meta->dmix_embedded,1);
        if (meta->primary_chset) put(b,0,3);
    }
    put(b,(uint32_t)(meta ? meta->hier_chset : 1),1);
    if (meta && meta->dmix_coeffs_present && !meta->primary_chset) {
        for (int row=0;row<meta->hier_ofs;row++) {
            put(b,meta->dmix_scale_code[row],9);
            for (int col=0;col<ch;col++) put(b,meta->dmix_coeff_code[row][col],9);
        }
    }
    put(b,1,1); /* channel mask enabled */
    put(b,meta ? meta->ch_mask : speaker_mask_layout(ch,stereo_lfe),
        meta ? meta->ch_mask_nbits : (ch > 6 ? 9 : 6));
    put(b,(uint32_t)decor_enabled,1);
    if (decor_enabled && ch > 1) {
        for (int c=0;c<ch;c++) put(b,(uint32_t)orig_order[c],order_bits);
        for (int pair=0;pair<ch/2;pair++) {
            put(b,decor_pairs[pair] != 0,1);
            if (decor_pairs[pair]) put(b,zigzag(decor_pairs[pair]),7);
        }
    }
    for (int k=0;k<ch;k++) put(b,(uint32_t)adaptive[k],4);
    for (int k=0;k<ch;k++) if (!adaptive[k]) put(b,(uint32_t)predict[k],2);
    for (int k=0;k<ch;k++)
        for (int j=0;j<adaptive[k];j++)
            put(b,zigzag(reflection[k][j]),8);
    if (split) {
        /* LSB section length is fixed for all segments, including any
         * padding. Since segment sizes are powers of two >=32, 32 samples
         * times an integral bit width always ends at a byte boundary. */
        if ((lsb_bits_per_segment*(unsigned)samples_per_segment)&7U) return -1;
        unsigned section=(lsb_bits_per_segment*(unsigned)samples_per_segment)/8U;
        if (seg_size_nbits>=32U || section >= (UINT32_C(1)<<seg_size_nbits)) return -1;
        put(b,section,seg_size_nbits);
        for(int c=0;c<ch;c++)put(b,lsb_width[c],4);
        for(int c=0;c<ch;c++)put(b,0,4); /* no lossy bit-width adjustment */
    }
    finish_crc(b,sub_start);
    if (b->error) return -1;
    patch(b,sub_size,10,(uint32_t)((b->position-sub_start)/8-1));
    patch(b,b->position-16,16,crc16(b->data+sub_start/8,
                        (b->position-sub_start)/8-2));

    size_t navi_start=b->position;
    size_t segsize[16];
    for (int seg=0;seg<nsegments;seg++) {segsize[seg]=b->position;put(b,0,seg_size_nbits);}
    finish_crc(b,navi_start);
    size_t navi_crc=b->position-16;
    if (b->error) return -1;

    for (int seg=0;seg<nsegments;seg++) {
        size_t seg_start=b->position;
        if (!compressed) {
            if (seg) put(b,(uint32_t)hdma_parameter_reuse,1);
            if (!seg || !hdma_parameter_reuse) {
                put(b,0,1); /* separate allocations: transformed widths may differ */
                for(int c=0;c<ch;c++) put(b,0,1); /* linear coding */
                for(int c=0;c<ch;c++) {
                    if(!seg) put(b,0,allocbits); /* no part A */
                    put(b,coding[seg][c].width-1U,allocbits);
                }
            }
        } else {
            if (seg) put(b,selected.reuse[seg],1);
            if (!seg || !selected.reuse[seg]) {
                put(b,selected.common[seg],1);
                int coded_channels=selected.common[seg] ? 1 : ch;
                for (int c=0;c<coded_channels;c++) {
                    put(b,coding[seg][c].rice,1);
                    if(!selected.common[seg] && coding[seg][c].rice) {
                        put(b,coding[seg][c].hybrid,1);
                        if(coding[seg][c].hybrid)
                            put(b,coding[seg][c].hybrid_width-1,allocbits);
                    }
                }
                for(int c=0;c<coded_channels;c++) {
                    if(!seg) {
                        unsigned wa=adaptive[c] ? part_a[c].width : 0;
                        put(b,coding[seg][c].rice ? wa : wa ? wa-1 : 0,allocbits);
                    }
                    unsigned width=coding[seg][c].width;
                    put(b,coding[seg][c].rice ? width : width ? width-1 : 0,allocbits);
                }
            }
        }
        for (int c=0;c<ch;c++) {
            Coding choice = coding[seg][c];
            unsigned start_b = seg==0 ? (unsigned)adaptive[c] : 0U;
            unsigned count_b = (unsigned)samples_per_segment - start_b;
            /* FFmpeg decodes part A BEFORE the isolated-sample index list.
             * Emitting the list first corrupts every adaptive-hybrid frame. */
            for (unsigned n=0;n<start_b;n++) {
                uint32_t code=zigzag(channel_data[c][seg*samples_per_segment+n]);
                if (part_a[c].rice) put_rice(b,code,part_a[c].width);
                else if (part_a[c].width) put(b,code,part_a[c].width);
            }
            if (compressed && choice.hybrid) {
                unsigned n_iso = 0;
                for (unsigned n=0;n<count_b;n++) {
                    uint32_t code=zigzag(channel_data[c][seg*samples_per_segment+start_b+n]);
                    uint64_t ricebits = (uint64_t)(code >> choice.width)+1+choice.width;
                    if (signed_width(code) <= choice.hybrid_width &&
                        ricebits > choice.hybrid_width + (segment_log2))
                        ++n_iso;
                }
                put(b,n_iso,segment_log2);
                for (unsigned n=0;n<count_b;n++) {
                    uint32_t code=zigzag(channel_data[c][seg*samples_per_segment+start_b+n]);
                    uint64_t ricebits=(uint64_t)(code>>choice.width)+1+choice.width;
                    if (signed_width(code)<=choice.hybrid_width &&
                        ricebits>choice.hybrid_width+(segment_log2))
                        put(b,n,segment_log2);
                }
            }
            for (int n=(int)start_b;n<samples_per_segment;n++) {
                uint32_t code=zigzag(channel_data[c][seg*samples_per_segment+n]);
                Coding selected=choice;
                int isolated=0;
                if (compressed && selected.hybrid && n >= (int)start_b) {
                    uint64_t ricebits=(uint64_t)(code>>selected.width)+1+selected.width;
                    isolated=signed_width(code)<=selected.hybrid_width &&
                        ricebits>selected.hybrid_width+(segment_log2);
                }
                if (isolated) put(b,code,selected.hybrid_width);
                else if (selected.rice) put_rice(b,code,selected.width);
                else if (selected.width) put(b,code,selected.width);
            }
        }
        if (split) {
            /* Each channel's original PCM LSBs follow the compressed MSB
             * portion in original (not decorrelated) speaker order. */
            align_byte(b);
            for(int c=0;c<ch;c++) {
                unsigned width=lsb_width[c];
                if(!width)continue;
                unsigned mask=(1U<<width)-1U;
                int input_ch=meta_input_channel(meta,ch,c,stereo_lfe);
                for(int n=0;n<samples_per_segment;n++) {
                    int idx=seg*samples_per_segment+n;
                    uint32_t v=(uint32_t)pcm_sample(pcm,ch,depth,input_ch,idx,raw_pcm);
                    put(b,v&mask,width);
                }
            }
        }
        align_byte(b);
        if (b->error || (b->position-seg_start)/8==0 || (b->position-seg_start)/8>16384) return -1;
        {
            size_t segment_bytes=(b->position-seg_start)/8U;
            if(largest_segment && segment_bytes>*largest_segment) *largest_segment=segment_bytes;
            if(segment_bytes-1U >= (UINT32_C(1)<<seg_size_nbits)) return -1;
            patch(b,segsize[seg],seg_size_nbits,(uint32_t)(segment_bytes-1U));
        }
    }
    patch(b,navi_crc,16,crc16(b->data+navi_start/8,
                 (navi_crc-navi_start)/8));
    while (b->position & 31) put(b,0,8);
    if (b->error || (b->position-start)/8>65536 || (b->position-start)/8==0) return -1;
    if((b->position-start)/8U-1U >= (UINT32_C(1)<<frame_size_nbits)) return -1;
    patch(b,frame_size,frame_size_nbits,(uint32_t)((b->position-start)/8U-1U));
    /* The frame size lives in the common header: finalize its CRC last. */
    patch(b,common_crc_pos,16,crc16(b->data+(start+32)/8,
                                      (common_crc_pos-start-32)/8));
    return (int)((b->position-start)/8);
}

static int make_xll(const int32_t *pcm,int ch,int rate,int depth,Bits *b,
                    int raw_pcm,unsigned full_mask,const unsigned lsb_width[8],
                    const XllSetMeta *meta,int stereo_lfe)
{
    if(!b || (b->position&7U) || b->position/8U>b->capacity) return -1;
    size_t capacity=b->capacity-b->position/8U;
    if(capacity>65536U) capacity=65536U; /* XLL frame-size limit */
    uint8_t *trial=calloc(capacity,1);
    if(!trial) return -1;
    /* The established single-set scalable-LSB mode keeps a 14-bit NAVI width
     * for backward bitstream stability. The section-size field itself follows
     * that selected segment width; hierarchical 7.1 independently minimizes
     * the shared width after measuring both channel sets. */
    int scalable=0;
    for(int c=0;c<ch;c++) if(lsb_width[c]) scalable=1;
    unsigned frame_bits=16U,segment_bits=14U;
    int size=-1,converged=0;
    size_t largest=0;
    for(int iteration=0;iteration<8;iteration++) {
        memset(trial,0,capacity);
        Bits tb={trial,capacity,0,0};
        largest=0;
        size=make_xll_once(pcm,ch,rate,depth,&tb,raw_pcm,full_mask,lsb_width,
                           meta,stereo_lfe,frame_bits,segment_bits,&largest);
        if(size<0 || tb.error || !largest) {free(trial);return -1;}
        unsigned next_frame=bits_required_u32((uint32_t)size-1U);
        unsigned next_segment=scalable ? 14U : bits_required_u32((uint32_t)largest-1U);
        if(next_frame==frame_bits && next_segment==segment_bits) {
            converged=1;
            break;
        }
        frame_bits=next_frame;
        segment_bits=next_segment;
    }
    if(!converged || bits_required_u32((uint32_t)size-1U)!=frame_bits ||
       (!scalable && bits_required_u32((uint32_t)largest-1U)!=segment_bits)) {free(trial);return -1;}
    /* EXSS headers emitted by this writer end on a 32-bit boundary, exactly
     * like a stand-alone XLL frame. Reuse the converged trial instead of
     * running the expensive predictor/entropy search a final time. */
    if((b->position&31U)==0U && b->position/8U+(size_t)size<=b->capacity) {
        memcpy(b->data+b->position/8U,trial,(size_t)size);
        b->position+=(size_t)size*8U;
        free(trial);
        return size;
    }
    /* Defensive fallback for a future caller with a different alignment. */
    largest=0;
    size=make_xll_once(pcm,ch,rate,depth,b,raw_pcm,full_mask,lsb_width,
                       meta,stereo_lfe,frame_bits,segment_bits,&largest);
    free(trial);
    if(size<0 || bits_required_u32((uint32_t)size-1U)!=frame_bits ||
       (!scalable && bits_required_u32((uint32_t)largest-1U)!=segment_bits)) return -1;
    return size;
}

static int hdma_xll_encode_mode(const int32_t *pcm,int channels,int rate,int depth,
                    uint8_t *output,size_t capacity,int raw_pcm,unsigned full_mask,
                    const unsigned lsb_width[8],int stereo_lfe)
{
    if (!pcm || !output || capacity<64 || !speaker_mask_layout(channels,stereo_lfe) ||
        (depth!=16 && depth!=24) || freq_code(rate)<0) return -1;
    Bits b={output,capacity,0,0};
    memset(output,0,capacity);
    /* EXSS container: a static, single-asset header followed by one XLL frame. */
    put(&b,0x64582025U,32);
    put(&b,0,8); /* user-defined bits */
    put(&b,0,2); /* extension substream index 0 */
    put(&b,0,1); /* short header */
    size_t exss_header=b.position;put(&b,0,8);
    size_t exss_length=b.position;put(&b,0,16);
    put(&b,1,1); /* stream static fields present */
    put(&b,(uint32_t)reference_clock_code(rate),2); /* reference clock family */
    put(&b,0,3); /* frame duration code (decoder infers sample count from XLL) */
    put(&b,0,1); /* no timecode */
    put(&b,0,3); /* one audio presentation */
    put(&b,0,3); /* one audio asset */
    put(&b,1,1); /* active EXSS mask */
    put(&b,1,8); /* active audio asset mask */
    put(&b,0,1); /* no mixing metadata */
    size_t asset_length=b.position;put(&b,0,16);
    size_t descriptor_start=b.position;
    size_t descriptor_length=b.position;put(&b,0,9);
    put(&b,0,3); /* audio asset index */
    put(&b,0,1); /* no asset type descriptor */
    put(&b,0,1); /* no language */
    put(&b,0,1); /* no additional text */
    put(&b,(uint32_t)(depth-1),5);
    put(&b,(uint32_t)freq_code(rate),4);
    put(&b,(uint32_t)(channels-1),8);
    put(&b,1,1); /* one-to-one channels and speakers */
    if (channels>2) put(&b,0,1); /* no embedded stereo */
    if (channels>6) put(&b,0,1); /* no embedded six-channel downmix */
    put(&b,1,1); /* speaker mask enabled */
    put(&b,channels > 6 ? 1 : 0,2); /* 8 or 4 speaker-pair bits */
    put(&b,pair_mask_layout(channels,stereo_lfe),channels > 6 ? 8 : 4);
    put(&b,0,3); /* no speaker remapping sets */
    put(&b,0,1); /* no dynamic range coefficients */
    put(&b,0,1); /* no dialog normalization */
    /* Explicitly describe both components (external core + XLL) instead of
     * relying on implicit XLL-only coding, preserving normal XLL tools. */
    put(&b,0U,2); /* explicit extension-mask coding mode */
    put(&b,0x201U,12);
    size_t xll_length=b.position;put(&b,0,16);
    put(&b,1,1); /* XLL sync present */
    put(&b,0,4); /* smoothing buffer */
    put(&b,0,5); /* decoding delay bits-1 = 0 */
    put(&b,0,1); /* initial decoding delay = 0 */
    put(&b,0,16); /* XLL sync offset */
    put(&b,0,3); /* DCA/XLL stream ID */
    /* Reserve the canonical descriptor length for supported layouts where
     * it fits; larger channel maps retain correct byte alignment. */
    size_t descriptor_target=descriptor_start+14U*8U;
    if(b.position<=descriptor_target) put(&b,0,(unsigned)(descriptor_target-b.position));
    else while ((b.position-descriptor_start)&7U) put(&b,0,1);
    if (b.error || (b.position-descriptor_start)/8>512) return -1;
    patch(&b,descriptor_length,9,(uint32_t)((b.position-descriptor_start)/8-1));
    put(&b,1U,1); /* core outside EXSS */
    put(&b,0,2); /* core substream index */
    put(&b,0,3); /* core asset index */
    put(&b,0,16); /* zero reserved extension-header bits */
    finish_crc(&b,40);
    if (b.error || b.position/8>256) return -1;
    size_t header_bytes=b.position/8;
    patch(&b,exss_header,8,(uint32_t)(header_bytes-1));
    size_t xll_start=b.position;
    int xll=make_xll(pcm,channels,rate,depth,&b,raw_pcm,full_mask,lsb_width,NULL,stereo_lfe);
    if (xll<0 || b.error || b.position/8>65536 || b.position/8>capacity) return -1;
    patch(&b,xll_length,16,(uint32_t)(xll-1));
    patch(&b,asset_length,16,(uint32_t)(xll-1));
    patch(&b,exss_length,16,(uint32_t)(b.position/8-1));
    (void)xll_start;
    /* CRC on EXSS header must be calculated AFTER header-size/asset-size patching. */
    uint16_t crc=crc16(b.data+5,header_bytes-5-2);
    patch(&b,(header_bytes-2)*8,16,crc);
    return (int)(b.position/8);
}

/* Scalable MSB/LSB coding is most useful when the lower bits contain
 * nearly incompressible noise. Enumerate useful split widths separately
 * for each channel; exact full-frame trials decide the winner. The original
 * no-split XLL remains the deterministic fallback. */
static int hdma_xll_encode_full(const int32_t *pcm,int channels,int rate,int depth,
                                uint8_t *output,size_t capacity,int ultra,int stereo_lfe)
{
    const unsigned zero[8]={0};
    int best=hdma_xll_encode_mode(pcm,channels,rate,depth,output,capacity,0,
                                  (1U<<channels)-1U,zero,stereo_lfe);
    if(best<0 || depth!=24 || !ultra)return best;
    uint8_t *trial=malloc(capacity);
    if(!trial)return best; /* no change to the known-good full PCM mode */
    unsigned candidates[5][8]={{0}};
    uint64_t estimate[5]={0},baseline_estimate=0;
    uint64_t uniform_estimate[3]={0};
    int num=0;
    static const unsigned widths[]={4,8,12};
    for(unsigned w=0;w<sizeof(widths)/sizeof(widths[0]);w++) {
        for(int c=0;c<channels;c++) candidates[num][c]=widths[w];
        num++;
    }
    /* Select a variable split depth for each speaker by accounting for the
     * baseline predictor estimate, each MSB predictor and the exact raw LSB
     * payload. This search does not depend on speaker order or core mapping. */
    const unsigned try_widths[]={0,2,4,6,8,10,12,15};
    for(int c=0;c<channels;c++) {
        int32_t source[HDMA_XLL_SAMPLES], shifted[HDMA_XLL_SAMPLES];
        for(int n=0;n<HDMA_XLL_SAMPLES;n++)source[n]=pcm_sample(pcm,channels,depth,
                                                  channel_index_layout(channels,c,stereo_lfe),n,0);
        uint64_t best_cost=UINT64_MAX;
        unsigned selected=0;
        for(unsigned t=0;t<sizeof(try_widths)/sizeof(try_widths[0]);t++) {
            unsigned w=try_widths[t];
            int64_t div=INT64_C(1)<<w;
            for(int n=0;n<HDMA_XLL_SAMPLES;n++) {
                int64_t v=source[n];
                shifted[n]=(int32_t)(v>=0 ? v/div : -((-v+div-1)/div));
            }
            ChannelPlan plan=plan_fixed(shifted,depth);
            uint64_t cost=plan.bits+(uint64_t)HDMA_XLL_SAMPLES*w+(w?4U:0U);
            if(!w) baseline_estimate+=cost;
            if(w==4)uniform_estimate[0]+=cost;
            if(w==8)uniform_estimate[1]+=cost;
            if(w==12)uniform_estimate[2]+=cost;
            if(cost<best_cost) {best_cost=cost;selected=w;}
        }
        candidates[num][c]=selected;
        estimate[num]+=best_cost;
    }
    num++;
    for(int k=0;k<3;k++)estimate[k]=uniform_estimate[k];
    /* A narrower split can help in some channels while a wider split helps
     * others. Test the independently chosen layout and all uniform layouts
     * with the actual writer, including segment headers and padding. */
    for(int k=0;k<num;k++) {
        int any=0,duplicate=0;
        for(int c=0;c<channels;c++)if(candidates[k][c])any=1;
        for(int j=0;j<k;j++) {
            int same=1;
            for(int c=0;c<channels;c++)
                if(candidates[j][c]!=candidates[k][c])same=0;
            if(same)duplicate=1;
        }
        if(!any || duplicate || estimate[k]>baseline_estimate+512U)continue;
        int size=hdma_xll_encode_mode(pcm,channels,rate,depth,trial,capacity,0,
                                      (1U<<channels)-1U,candidates[k],stereo_lfe);
        if(size>=0 && size<best) {best=size;memcpy(output,trial,(size_t)size);}
    }
    free(trial);
    return best;
}

int hdma_xll_encode(const int32_t *pcm,int channels,int rate,int depth,
                    uint8_t *output,size_t capacity)
{
    if(!pcm || !output || !speaker_mask_layout(channels,0) || (depth!=16 && depth!=24)
       || capacity<64)return -1;
    return hdma_xll_encode_full(pcm,channels,rate,depth,output,capacity,1,0);
}
/* Explicit FL/FR/LFE mapping, including both EXSS masks and the XLL set. */
int hdma_xll_encode_21(const int32_t *pcm,int rate,int depth,uint8_t *output,size_t capacity)
{
    if(!pcm || !output || capacity<64 || (depth!=16 && depth!=24))return -1;
    return hdma_xll_encode_full(pcm,3,rate,depth,output,capacity,1,1);
}

static int popcount_mask(unsigned mask)
{
    int count=0;
    while (mask) {count+=(int)(mask&1U);mask>>=1;}
    return count;
}

/* Select core-reference flags independently in XLL speaker-channel order.
 * Unlike frame-wide fallback, channels with overflowing core residuals can
 * be encoded as full PCM while other channels still reference the core.
 * The XLL decoder inverts pair decorrelation BEFORE adding the core, so
 * decorrelation of a mixed full/residual representation remains reversible. */
int hdma_xll_encode_core_mode_options(const int32_t *pcm,const int32_t *core_pcm,
                              int channels,int rate,int depth,
                              uint8_t *output,size_t capacity,int mode,int *used_core,int ultra)
{
    if (used_core) *used_core=0;
    if (mode==HDMA_XLL_REF_FULL)
        return hdma_xll_encode_full(pcm,channels,rate,depth,output,capacity,ultra,0);
    if (!pcm || !core_pcm || !output || !speaker_mask_layout(channels,0) ||
        (depth!=16 && depth!=24) || capacity<64 ||
        (mode!=HDMA_XLL_REF_AUTO && mode!=HDMA_XLL_REF_CORE)) return -1;
    int32_t originals[HDMA_XLL_SAMPLES*8], differences[HDMA_XLL_SAMPLES*8], combined[HDMA_XLL_SAMPLES*8];
    uint64_t full_cost[8]={0}, core_cost[8]={0};
    unsigned full=(1U<<channels)-1U;
    /* A 7.x stream has a 5.x core. Its Ls/Rs speaker slots correspond to
     * the downmixed core surrounds; the additional Lsr/Rsr speakers have
     * no core counterparts and MUST always use full PCM. The caller maps
     * reconstructed core samples to XLL speakers, never by array position. */
    unsigned forced_full = channels==7 ? 0x60U : channels==8 ? 0xc0U : 0U;
    unsigned heuristic_full=0;
    int32_t minval=-(int32_t)(1U<<(depth-1));
    int32_t maxval=(int32_t)((1U<<(depth-1))-1U);
    for (int c=0;c<channels;c++) {
        int input_ch=channel_index_layout(channels,c,0);
        int eligible=!(forced_full & (1U<<c));
        int32_t source[HDMA_XLL_SAMPLES], delta[HDMA_XLL_SAMPLES];
        for (int n=0;n<HDMA_XLL_SAMPLES;n++) {
            size_t pos=(size_t)n*channels+input_ch;
            int32_t original=depth==16 ? pcm[pos]/65536 : pcm[pos]/256;
            int32_t decoded=depth==16 ? (core_pcm[pos]+128)>>8 : core_pcm[pos];
            int64_t diff=(int64_t)original-decoded;
            originals[pos]=source[n]=original;
            if (diff<minval || diff>maxval) eligible=0;
            differences[pos]=delta[n]=(int32_t)diff;
        }
        if (!eligible) {
            forced_full |= 1U<<c;
            continue;
        }
        if (mode==HDMA_XLL_REF_AUTO) {
            /* Evaluate every legal full/core combination using channel-wise
             * predictor costs, then encode a bounded shortlist exactly. */
            ChannelPlan p=plan_fixed(source,depth),r=plan_fixed(delta,depth);
            full_cost[c]=p.bits;
            core_cost[c]=r.bits;
            if (p.bits<=r.bits) heuristic_full |= 1U<<c;
        }
    }
    if (mode==HDMA_XLL_REF_CORE) {
        /* In 7.x CORE means every *available* speaker must reference core;
         * the two extra rears are obligatorily full. A residual overflow on
         * any otherwise available speaker is still a hard failure. */
        unsigned unavailable=channels==7 ? 0x60U : channels==8 ? 0xc0U : 0U;
        if (forced_full != unavailable) return -2;
        for (int c=0;c<channels;c++) {
            int input_ch=channel_index_layout(channels,c,0);
            int is_full=(forced_full>>c)&1U;
            for (int n=0;n<HDMA_XLL_SAMPLES;n++) {
                size_t pos=(size_t)n*channels+input_ch;
                combined[pos]=is_full?originals[pos]:differences[pos];
            }
        }
        int size=hdma_xll_encode_mode(combined,channels,rate,depth,
                                      output,capacity,1,forced_full,(const unsigned[8]){0},0);
        if (size>=0 && used_core) *used_core=channels-popcount_mask(forced_full);
        return size;
    }
    /* Exhaustively score all 2^channels channel-reference masks. Invalid
     * core residuals are never considered. Fully encode a bounded shortlist
     * plus the three legacy candidates: bitstream-level decorrelation and
     * segmentation are non-additive, so estimated rank is not a guarantee
     * of the globally optimal combination. */
    unsigned masks[16], nmasks=0;
    const unsigned mandatory[3]={full,forced_full,forced_full|heuristic_full};
    for (unsigned i=0;i<3;i++) {
        int found=0;
        for(unsigned j=0;j<nmasks;j++) if(masks[j]==mandatory[i]) found=1;
        if(!found) masks[nmasks++]=mandatory[i];
    }
    uint64_t ranked[4];
    unsigned ranked_masks[4];
    unsigned rank_count=0;
    /* Ordinary encoding only tests full, eligible same-channel core and one
     * cheap independently estimated mixture. Exhaustive mask scoring is
     * available with --ultra; no cross-speaker core reference is attempted. */
    if (ultra) for (unsigned mask=0; mask<=full;mask++) {
        if ((mask&forced_full)!=forced_full) continue;
        uint64_t cost=0;
        for (int c=0;c<channels;c++)
            cost += (mask&(1U<<c))?full_cost[c]:core_cost[c];
        /* Prefer fewer mask transitions when per-channel costs tie; this
         * approximates the decorrelation benefit of adjacent speakers. */
        cost += (uint64_t)8*popcount_mask((mask^(mask>>1))&((1U<<(channels-1))-1));
        unsigned insert=0;
        while(insert<rank_count && (ranked[insert]<cost ||
              (ranked[insert]==cost && ranked_masks[insert]<mask))) insert++;
        if(insert>=4) continue;
        if(rank_count<4)rank_count++;
        for(unsigned i=rank_count-1;i>insert;i--) {
            ranked[i]=ranked[i-1]; ranked_masks[i]=ranked_masks[i-1];
        }
        ranked[insert]=cost;ranked_masks[insert]=mask;
    }
    for(unsigned i=0;i<rank_count;i++) {
        int found=0;
        for(unsigned j=0;j<nmasks;j++) if(masks[j]==ranked_masks[i])found=1;
        if(!found)masks[nmasks++]=ranked_masks[i];
    }
    uint8_t *trial=malloc(capacity);
    if (!trial) return -1;
    int best_size=-1, best_cores=0;
    for (unsigned option=0;option<nmasks;option++) {
        unsigned mask=masks[option];
        int size;
        if (mask==full) {
            size=hdma_xll_encode_full(pcm,channels,rate,depth,trial,capacity,ultra,0);
        } else {
            for (int c=0;c<channels;c++) {
                int input_ch=channel_index_layout(channels,c,0);
                int is_full=(mask>>c)&1U;
                for (int n=0;n<HDMA_XLL_SAMPLES;n++) {
                    size_t pos=(size_t)n*channels+input_ch;
                    combined[pos]=is_full?originals[pos]:differences[pos];
                }
            }
            size=hdma_xll_encode_mode(combined,channels,rate,depth,trial,
                                       capacity,1,mask,(const unsigned[8]){0},0);
        }
        if (size>=0 && (best_size<0 || size<best_size)) {
            best_size=size;
            best_cores=channels-popcount_mask(mask);
            memcpy(output,trial,(size_t)size);
        }
    }
    free(trial);
    if (used_core && best_size>=0) *used_core=best_cores;
    return best_size;
}

int hdma_xll_encode_core_mode(const int32_t *pcm,const int32_t *core_pcm,
                              int channels,int rate,int depth,
                              uint8_t *output,size_t capacity,int mode,int *used_core)
{
    return hdma_xll_encode_core_mode_options(pcm,core_pcm,channels,rate,depth,
                                              output,capacity,mode,used_core,1);
}

int hdma_xll_encode_core(const int32_t *pcm,const int32_t *core_pcm,
                         int channels,int rate,int depth,
                         uint8_t *output,size_t capacity)
{
    return hdma_xll_encode_core_mode(pcm,core_pcm,channels,rate,depth,
                                     output,capacity,HDMA_XLL_REF_AUTO,NULL);
}

/* -------------------------------------------------------------------------
 * Hierarchical 7.0/7.1 XLL
 *
 * A reference-style stream carries a primary 5.0/5.1 set whose surround
 * identities are Lss/Rss and a secondary Lsr/Rsr set.  The secondary header
 * signals the reversible matrix used to form the compatibility core.  The
 * helpers below deliberately reuse the ordinary single-set planner and only
 * assemble the two already-validated channel-set payloads afterwards.
 */
typedef struct {
    const uint8_t *subheader;
    size_t subheader_bytes;
    const uint8_t *segment[16];
    size_t segment_bytes[16];
    unsigned nsegments;
    unsigned grid_log;
    unsigned segment_log2;
    unsigned seg_size_nbits;
    unsigned scalable_lsbs;
} XllComponent;

typedef struct {
    const uint8_t *data;
    size_t size_bits;
    size_t pos;
    int error;
} ReadBits;

static uint32_t read_bits(ReadBits *r,unsigned width)
{
    uint32_t v=0;
    if(width>32 || r->error || r->pos+width>r->size_bits) {
        r->error=1; return 0;
    }
    while(width--) {
        v=(v<<1)|((r->data[r->pos>>3]>>(7-(r->pos&7)))&1U);
        r->pos++;
    }
    return v;
}

static int parse_single_component(const uint8_t *data,size_t bytes,XllComponent *out)
{
    ReadBits r={data,bytes*8,0,0};
    memset(out,0,sizeof(*out));
    if(read_bits(&r,32)!=0x41a29547U) return -1;
    read_bits(&r,4);
    unsigned common_bytes=read_bits(&r,8)+1U;
    unsigned frame_bits=read_bits(&r,5)+1U;
    read_bits(&r,frame_bits);
    if(read_bits(&r,4)!=0) return -1;
    out->grid_log=read_bits(&r,4);
    out->segment_log2=read_bits(&r,4);
    out->seg_size_nbits=read_bits(&r,5)+1U;
    read_bits(&r,2);
    out->scalable_lsbs=read_bits(&r,1);
    if(out->scalable_lsbs) read_bits(&r,4); /* variable per-channel LSB widths */
    unsigned mask_bits=read_bits(&r,5)+1U;
    (void)mask_bits;
    if(r.error || common_bytes>bytes) return -1;
    r.pos=(size_t)common_bytes*8U;
    size_t sub_start=r.pos;
    unsigned sub_bytes=read_bits(&r,10)+1U;
    if(r.error || sub_start/8U+sub_bytes>bytes) return -1;
    out->subheader=data+sub_start/8U;
    out->subheader_bytes=sub_bytes;
    r.pos=sub_start+(size_t)sub_bytes*8U;
    out->nsegments=1U<<out->grid_log;
    if(out->nsegments>16 || out->seg_size_nbits>24) return -1;
    size_t sizes[16];
    for(unsigned i=0;i<out->nsegments;i++) sizes[i]=read_bits(&r,out->seg_size_nbits)+1U;
    if(r.error) return -1;
    r.pos=(r.pos+7U)&~(size_t)7U;
    r.pos+=16U;
    if(r.pos>bytes*8U) return -1;
    size_t offset=r.pos/8U;
    for(unsigned i=0;i<out->nsegments;i++) {
        if(offset+sizes[i]>bytes) return -1;
        out->segment[i]=data+offset;
        out->segment_bytes[i]=sizes[i];
        offset+=sizes[i];
    }
    return 0;
}

static int encode_component(const int32_t *pcm,int channels,int rate,int depth,
                            int raw_pcm,unsigned full_mask,const XllSetMeta *meta,
                            unsigned frame_size_nbits,unsigned seg_size_nbits,
                            uint8_t *output,size_t capacity)
{
    Bits b={output,capacity,0,0};
    const unsigned no_lsb[8]={0};
    memset(output,0,capacity);
    size_t largest=0;
    int n=make_xll_once(pcm,channels,rate,depth,&b,raw_pcm,full_mask,no_lsb,
                        meta,0,frame_size_nbits,seg_size_nbits,&largest);
    return n<0 || b.error ? -1 : n;
}

static void put_bytes(Bits *b,const uint8_t *src,size_t count)
{
    if((b->position&7U) || b->error || count>b->capacity-(b->position>>3)) {
        b->error=1; return;
    }
    memcpy(b->data+(b->position>>3),src,count);
    b->position+=count*8U;
}

static int combine_components_once(const XllComponent *p,const XllComponent *s,
                                   unsigned frame_size_nbits,
                                   uint8_t *output,size_t capacity)
{
    if(!p || !s || frame_size_nbits<1U || frame_size_nbits>20U ||
       p->nsegments!=s->nsegments || p->grid_log!=s->grid_log ||
       p->segment_log2!=s->segment_log2 || p->seg_size_nbits!=s->seg_size_nbits ||
       p->scalable_lsbs!=s->scalable_lsbs) return -1;
    Bits b={output,capacity,0,0};
    memset(output,0,capacity);
    size_t start=b.position;
    put(&b,0x41a29547U,32);
    put(&b,0,4);
    size_t common_size=b.position; put(&b,0,8);
    put(&b,frame_size_nbits-1U,5);
    size_t frame_size=b.position; put(&b,0,frame_size_nbits);
    put(&b,1,4); /* two channel sets */
    put(&b,p->grid_log,4);
    put(&b,p->segment_log2,4);
    put(&b,p->seg_size_nbits-1U,5);
    put(&b,0,2);
    put(&b,p->scalable_lsbs,1);
    put(&b,10,5); /* eleven speaker-mask bits, through Rss */
    if(p->scalable_lsbs) put(&b,0,4); /* variable per-channel LSB widths */
    finish_crc(&b,start+32);
    size_t common_crc=b.position-16U;
    if(b.error) return -1;
    patch(&b,common_size,8,(uint32_t)((b.position-start)/8U-1U));
    patch(&b,common_crc,16,crc16(b.data+(start+32U)/8U,
          (common_crc-start-32U)/8U));

    put_bytes(&b,p->subheader,p->subheader_bytes);
    put_bytes(&b,s->subheader,s->subheader_bytes);
    size_t navi_start=b.position;
    for(unsigned seg=0;seg<p->nsegments;seg++) {
        put(&b,(uint32_t)(p->segment_bytes[seg]-1U),p->seg_size_nbits);
        put(&b,(uint32_t)(s->segment_bytes[seg]-1U),p->seg_size_nbits);
    }
    finish_crc(&b,navi_start);
    if(b.error) return -1;
    for(unsigned seg=0;seg<p->nsegments;seg++) {
        put_bytes(&b,p->segment[seg],p->segment_bytes[seg]);
        put_bytes(&b,s->segment[seg],s->segment_bytes[seg]);
    }
    while(b.position&31U) put(&b,0,8);
    if(b.error || (b.position-start)/8U>65536U) return -1;
    size_t frame_bytes=(b.position-start)/8U;
    if(frame_bytes==0 || frame_bytes-1U >= (UINT32_C(1)<<frame_size_nbits)) return -2;
    patch(&b,frame_size,frame_size_nbits,(uint32_t)(frame_bytes-1U));
    patch(&b,common_crc,16,crc16(b.data+(start+32U)/8U,
          (common_crc-start-32U)/8U));
    return (int)frame_bytes;
}

static int combine_components(const uint8_t *a,size_t asz,const uint8_t *d,size_t dsz,
                              uint8_t *output,size_t capacity)
{
    XllComponent p,s;
    if(parse_single_component(a,asz,&p) || parse_single_component(d,dsz,&s) ||
       p.nsegments!=s.nsegments || p.grid_log!=s.grid_log ||
       p.segment_log2!=s.segment_log2 || p.seg_size_nbits!=s.seg_size_nbits ||
       p.scalable_lsbs!=s.scalable_lsbs)
        return -1;
    /* The supplied working 7.1 stream uses the minimum legal frame-size field
     * width.  Try widths in ascending order; changing this field can shift the
     * common-header byte alignment, so derive the final width from the fully
     * assembled frame rather than from a rough byte-count estimate. */
    for(unsigned frame_bits=7;frame_bits<=20U;frame_bits++) {
        int n=combine_components_once(&p,&s,frame_bits,output,capacity);
        if(n>=0) return n;
        if(n!=-2) return -1;
    }
    return -1;
}

static int wrap_hierarchical_xll(const uint8_t *xll,size_t xll_size,int channels,
                                 int rate,int depth,uint8_t *output,size_t capacity)
{
    Bits b={output,capacity,0,0};
    memset(output,0,capacity);
    put(&b,0x64582025U,32);
    put(&b,0,8); put(&b,0,2); put(&b,0,1);
    size_t exss_header=b.position; put(&b,0,8);
    size_t exss_length=b.position; put(&b,0,16);
    put(&b,1,1); put(&b,(uint32_t)reference_clock_code(rate),2); put(&b,0,3); put(&b,0,1);
    put(&b,0,3); put(&b,0,3); put(&b,1,1); put(&b,1,8); put(&b,0,1);
    size_t asset_length=b.position; put(&b,0,16);
    size_t descriptor_start=b.position;
    size_t descriptor_length=b.position; put(&b,0,9);
    put(&b,0,3); put(&b,0,1); put(&b,0,1); put(&b,0,1);
    put(&b,(uint32_t)(depth-1),5); put(&b,(uint32_t)freq_code(rate),4);
    put(&b,(uint32_t)(channels-1),8); put(&b,1,1);
    if(channels>2) put(&b,0,1);
    if(channels>6) put(&b,1,1); /* embedded six-channel downmix exists */
    put(&b,1,1);
    put(&b,2,2); /* twelve speaker-pair bits */
    put(&b,channels==8 ? 0x84bU : 0x843U,12);
    put(&b,0,3); put(&b,0,1); put(&b,0,1);
    /* Match the explicit external-core + XLL asset structure used by the
     * supplied working 7.1 DCA + XLL reference and by our established
     * single-set writer.  The old hierarchical wrapper used implicit
     * XLL-only coding and produced a shorter EXSS descriptor that FFmpeg
     * accepted but hardware receivers need not interpret identically. */
    put(&b,0,2);       /* explicit extension-mask coding mode */
    put(&b,0x201U,12); /* external DCA core + XLL */
    size_t xll_length=b.position; put(&b,0,16);
    /* The supplied working 7.1 reference uses smoothing-buffer code 8. */
    put(&b,1,1); put(&b,8,4); put(&b,0,5); put(&b,0,1);
    put(&b,0,16); put(&b,0,3);
    size_t descriptor_target=descriptor_start+14U*8U;
    if(b.position<=descriptor_target) put(&b,0,(unsigned)(descriptor_target-b.position));
    else while((b.position-descriptor_start)&7U) put(&b,0,1);
    if(b.error || (b.position-descriptor_start)/8U>512U) return -1;
    patch(&b,descriptor_length,9,(uint32_t)((b.position-descriptor_start)/8U-1U));
    put(&b,1,1); /* core outside EXSS */
    put(&b,0,2); /* core substream index */
    put(&b,0,3); /* core asset index */
    /* With the 15-byte 7.1 asset descriptor the observed stream has eight
     * reserved extension-header bits, yielding the same 32-byte EXSS header
     * as the supplied reference. */
    put(&b,0,8);
    finish_crc(&b,40);
    if(b.error || b.position/8U>256U) return -1;
    size_t header_bytes=b.position/8U;
    patch(&b,exss_header,8,(uint32_t)(header_bytes-1U));
    put_bytes(&b,xll,xll_size);
    if(b.error || b.position/8U>65536U) return -1;
    patch(&b,xll_length,16,(uint32_t)(xll_size-1U));
    patch(&b,asset_length,16,(uint32_t)(xll_size-1U));
    patch(&b,exss_length,16,(uint32_t)(b.position/8U-1U));
    patch(&b,(header_bytes-2U)*8U,16,crc16(b.data+5,header_bytes-7U));
    return (int)(b.position/8U);
}

static void init_hier_meta(XllSetMeta *primary,XllSetMeta *secondary,
                           int channels)
{
    memset(primary,0,sizeof(*primary));
    memset(secondary,0,sizeof(*secondary));
    const int pc=channels-2;
    primary->forced_grid_log=-1;
    primary->ch_mask_nbits=11;
    primary->ch_mask=channels==8 ? 0x627U : 0x607U;
    primary->primary_chset=1; primary->hier_chset=1;
    primary->force_scalable_lsb=1;
    if(channels==8) {
        int order[6]={2,0,1,3,4,5};
        memcpy(primary->input_order,order,sizeof(order));
    } else {
        int order[5]={2,0,1,3,4};
        memcpy(primary->input_order,order,sizeof(order));
    }
    secondary->forced_grid_log=-1;
    secondary->ch_mask_nbits=11; secondary->ch_mask=0x180U;
    secondary->primary_chset=0; secondary->hier_chset=1;
    secondary->force_scalable_lsb=1;
    secondary->dmix_coeffs_present=1; secondary->dmix_embedded=1;
    secondary->hier_ofs=pc;
    secondary->input_order[0]=0; secondary->input_order[1]=1;
    /* Match the supplied known-good 7.1 hierarchy exactly: all six embedded
     * 5.1 primary rows carry D.11 scale code 473 (about -3 dB), and Lsr/Rsr
     * feed only the corresponding Lss/Rss row with the same code.  Because
     * the coefficient and row scale are identical, full 7.1 reconstruction
     * subtracts the rear channel at unity while 5.1 playback scales the
     * primary set by -3 dB. */
    for(int row=0;row<pc;row++) {
        secondary->dmix_scale_code[row]=473;
        secondary->dmix_coeff_code[row][0]=256;
        secondary->dmix_coeff_code[row][1]=256;
    }
    secondary->dmix_coeff_code[pc-2][0]=473;
    secondary->dmix_coeff_code[pc-1][1]=473;
}

/* Build the reversible hierarchical primary in NATIVE PCM units, not the
 * left-aligned int32 container representation used by libsndfile.  The two
 * embedded surround intermediates are side+rear and can legitimately require
 * 25 signed bits for a 24-bit source.  Left-aligning those intermediates by
 * eight bits overflows int32_t before the XLL writer ever sees them. */
static int32_t hier_mul15(int32_t sample,int32_t coeff)
{
    return (int32_t)(((int64_t)sample*coeff + 16384) >> 15);
}

static int build_hierarchical_native(const int32_t *pcm,int channels,int depth,
                                     int32_t *primary,int32_t *secondary)
{
    if(!pcm || !primary || !secondary || channels!=8 || depth!=24) return -1;
    const int pc=6;
    for(int n=0;n<HDMA_XLL_SAMPLES;n++) {
        int32_t fl=pcm_sample(pcm,channels,depth,0,n,0);
        int32_t fr=pcm_sample(pcm,channels,depth,1,n,0);
        int32_t fc=pcm_sample(pcm,channels,depth,2,n,0);
        int32_t lfe=pcm_sample(pcm,channels,depth,3,n,0);
        int32_t bl=pcm_sample(pcm,channels,depth,4,n,0);
        int32_t br=pcm_sample(pcm,channels,depth,5,n,0);
        int32_t sl=pcm_sample(pcm,channels,depth,6,n,0);
        int32_t sr=pcm_sample(pcm,channels,depth,7,n,0);
        /* Match the decoder's actual hierarchy undo coefficient. Code 473
         * scaled by inverse(code 473) becomes 32767 in Q15, so the reversible
         * primary is side + mul15(rear,32767), not an exact side+rear sum. */
        int64_t lsum=(int64_t)sl+hier_mul15(bl,32767);
        int64_t rsum=(int64_t)sr+hier_mul15(br,32767);
        if(lsum<INT32_MIN || lsum>INT32_MAX || rsum<INT32_MIN || rsum>INT32_MAX) return -1;
        primary[(size_t)n*pc+0]=fl;
        primary[(size_t)n*pc+1]=fr;
        primary[(size_t)n*pc+2]=fc;
        primary[(size_t)n*pc+3]=lfe;
        primary[(size_t)n*pc+4]=(int32_t)lsum;
        primary[(size_t)n*pc+5]=(int32_t)rsum;
        secondary[(size_t)n*2]=bl;
        secondary[(size_t)n*2+1]=br;
    }
    return pc;
}

static int32_t hier_clip23(int64_t value)
{
    if(value>8388607) return 8388607;
    if(value<-8388608) return -8388608;
    return (int32_t)value;
}

static int32_t hier_mul16(int32_t sample,int32_t coeff)
{
    /* Match FFmpeg dcamath.h norm16/mul16 rounding exactly. */
    return (int32_t)(((int64_t)sample*coeff + 32768) >> 16);
}

static int build_hierarchical_candidate(const int32_t *pcm,const int32_t *core_pcm,
                                        int channels,int rate,int depth,int use_core,
                                        HdmaSidePolicy policy,uint8_t *output,size_t capacity)
{
    const int pc=channels-2;
    int32_t primary_pcm[HDMA_XLL_SAMPLES*6];
    int32_t secondary_pcm[HDMA_XLL_SAMPLES*2];
    (void)policy; /* lossy-core side policy must not alter XLL receiver downmix */
    if(build_hierarchical_native(pcm,channels,depth,primary_pcm,secondary_pcm)!=pc) return -1;
    XllSetMeta pm,sm;
    init_hier_meta(&pm,&sm,channels);
    int32_t encoded_primary[HDMA_XLL_SAMPLES*6];
    unsigned full_mask=(1U<<pc)-1U;
    int raw=1; /* full primary is already in native sample units */
    if(use_core) {
        if(!core_pcm || depth!=24) return -2;
        for(int n=0;n<HDMA_XLL_SAMPLES;n++) for(int c=0;c<pc;c++) {
            int32_t core_native=core_pcm[(size_t)n*pc+c];
            /* A hierarchical residual does NOT reference the attenuated core
             * directly.  DCA/XLL decoders first undo the embedded -3 dB core
             * pre-scale using the inverse for code 473, then clip to 24 bits.
             * Form our residual against that exact value so the stored primary
             * remains lossless while every residual still fits 24-bit syntax,
             * even when side+rear itself needs 25 bits. */
            int32_t reference=hier_clip23(hier_mul16(core_native,92682));
            int64_t diff=(int64_t)primary_pcm[(size_t)n*pc+c]-reference;
            if(diff < -8388608LL || diff > 8388607LL) return -2;
            encoded_primary[(size_t)n*pc+c]=(int32_t)diff;
        }
        full_mask=0; raw=1;
    } else memcpy(encoded_primary,primary_pcm,(size_t)HDMA_XLL_SAMPLES*pc*sizeof(int32_t));

    uint8_t *pa=malloc(capacity),*sa=malloc(capacity),*combined=malloc(capacity);
    if(!pa || !sa || !combined) { free(pa);free(sa);free(combined);return -1; }
    int saved_force_raw=hdma_force_raw;
    /* A self-contained full primary can contain a 25-bit surround sum and is
     * kept only as a legal fallback/diagnostic.  The normal receiver path uses
     * 24-bit core residuals and therefore normal XLL prediction/entropy tools.
     * raw_pcm=1 here only means the temporary values are already native-width;
     * it does not force raw-linear entropy coding. */
    if(!use_core) hdma_force_raw=1;
    /* Generate once with generous size fields to discover the chosen grid and
     * actual segment payload lengths, then regenerate both channel sets with
     * the minimum shared segment-size field width.  The final combined frame
     * independently minimizes its frame-size field. */
    int psz=encode_component(encoded_primary,pc,rate,depth,raw,full_mask,&pm,20U,20U,pa,capacity);
    hdma_force_raw=saved_force_raw;
    XllComponent parsed_p,parsed_s;
    if(psz<0 || parse_single_component(pa,(size_t)psz,&parsed_p)) {
        free(pa);free(sa);free(combined);return -1;
    }
    sm.forced_grid_log=(int)parsed_p.grid_log;
    int ssz=encode_component(secondary_pcm,2,rate,depth,1,3U,&sm,20U,20U,sa,capacity);
    if(ssz<0 || parse_single_component(sa,(size_t)ssz,&parsed_s)) {
        free(pa);free(sa);free(combined);return -1;
    }
    size_t largest=0;
    for(unsigned seg=0;seg<parsed_p.nsegments;seg++) {
        if(parsed_p.segment_bytes[seg]>largest) largest=parsed_p.segment_bytes[seg];
        if(parsed_s.segment_bytes[seg]>largest) largest=parsed_s.segment_bytes[seg];
    }
    if(!largest) {free(pa);free(sa);free(combined);return -1;}
    unsigned seg_bits=bits_required_u32((uint32_t)largest-1U);
    if(seg_bits<7U) seg_bits=7U;
    if(seg_bits>20U) {free(pa);free(sa);free(combined);return -1;}

    if(!use_core) hdma_force_raw=1;
    psz=encode_component(encoded_primary,pc,rate,depth,raw,full_mask,&pm,20U,seg_bits,pa,capacity);
    hdma_force_raw=saved_force_raw;
    if(psz<0 || parse_single_component(pa,(size_t)psz,&parsed_p)) {
        free(pa);free(sa);free(combined);return -1;
    }
    sm.forced_grid_log=(int)parsed_p.grid_log;
    ssz=encode_component(secondary_pcm,2,rate,depth,1,3U,&sm,20U,seg_bits,sa,capacity);
    if(ssz<0 || parse_single_component(sa,(size_t)ssz,&parsed_s)) {
        free(pa);free(sa);free(combined);return -1;
    }
    /* Segment coding itself is independent of NAVI field width; assert the
     * discovery pass was sufficient instead of silently truncating a size. */
    size_t limit=UINT32_C(1)<<seg_bits;
    for(unsigned seg=0;seg<parsed_p.nsegments;seg++)
        if(parsed_p.segment_bytes[seg]>limit || parsed_s.segment_bytes[seg]>limit) {
            free(pa);free(sa);free(combined);return -1;
        }
    int csz=combine_components(pa,(size_t)psz,sa,(size_t)ssz,combined,capacity);
    int result=csz<0 ? -1 : wrap_hierarchical_xll(combined,(size_t)csz,channels,rate,depth,output,capacity);
    free(pa);free(sa);free(combined);
    return result;
}

int hdma_xll_encode_hierarchical_7x_policy_options(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy,int hierarchy_mode)
{
    if(used_core) *used_core=0;
    if(!pcm || !output || (channels!=7 && channels!=8) ||
       side_policy<HDMA_SIDE_BOTH || side_policy>HDMA_SIDE_MUTE ||
       hierarchy_mode<HDMA_XLL_HIER_OFF || hierarchy_mode>HDMA_XLL_HIER_AUTO) return -1;
    /* The receiver-compatible 7.1 hierarchy intentionally mirrors the supplied
     * reference: a core-referenced six-channel primary plus full Lsr/Rsr. */
    /* The established single-set form is the hardware-compatible baseline.
     * It also remains the only form for 7.0, 16-bit 7.1 and 1024-sample XLL. */
    int32_t mapped[HDMA_XLL_SAMPLES*8];
    memset(mapped,0,sizeof(mapped));
    if(core_pcm) {
        const int pc=channels-2;
        const int side=channels==8?6:5;
        for(int n=0;n<HDMA_XLL_SAMPLES;n++) {
            mapped[(size_t)n*channels+0]=core_pcm[(size_t)n*pc+0];
            mapped[(size_t)n*channels+1]=core_pcm[(size_t)n*pc+1];
            mapped[(size_t)n*channels+2]=core_pcm[(size_t)n*pc+2];
            if(channels==8) mapped[(size_t)n*channels+3]=core_pcm[(size_t)n*pc+3];
            mapped[(size_t)n*channels+side]=core_pcm[(size_t)n*pc+pc-2];
            mapped[(size_t)n*channels+side+1]=core_pcm[(size_t)n*pc+pc-1];
        }
    }
    int fallback_used=0;
    int best=hdma_xll_encode_core_mode_options(pcm,
        mode==HDMA_XLL_REF_FULL?NULL:mapped,channels,rate,depth,
        output,capacity,mode,&fallback_used,ultra);
    if(best<0 && mode!=HDMA_XLL_REF_CORE) return best;
    if(hierarchy_mode==HDMA_XLL_HIER_OFF ||
       channels!=8 || depth!=24 || HDMA_XLL_SAMPLES!=512) {
        if(used_core) *used_core=fallback_used;
        return best;
    }

    uint8_t *trial=malloc(capacity);
    uint8_t *full_trial=malloc(capacity);
    if(!trial || !full_trial) { free(trial); free(full_trial); return best; }

    /* A stable hierarchical stream never falls back to a one-set frame merely
     * because the source amplitude or entropy makes that frame larger. AUTO is
     * retained only to reproduce the old frame-by-frame size switching. */
    if(hierarchy_mode==HDMA_XLL_HIER_ON) {
        int full_size=-1, core_size=-1;
        if(mode!=HDMA_XLL_REF_CORE || !core_pcm)
            full_size=build_hierarchical_candidate(pcm,core_pcm,channels,rate,depth,0,
                                                    (HdmaSidePolicy)side_policy,full_trial,capacity);
        if(mode!=HDMA_XLL_REF_FULL && core_pcm)
            core_size=build_hierarchical_candidate(pcm,core_pcm,channels,rate,depth,1,
                                                    (HdmaSidePolicy)side_policy,trial,capacity);
        if(mode==HDMA_XLL_REF_CORE) {
            if(core_size==-2) { free(trial);free(full_trial);return -2; }
            if(core_size<0) { free(trial);free(full_trial);return core_size; }
            memcpy(output,trial,(size_t)core_size);best=core_size;fallback_used=channels-2;
        } else if(mode==HDMA_XLL_REF_FULL || !core_pcm) {
            if(full_size<0) { free(trial);free(full_trial);return full_size; }
            memcpy(output,full_trial,(size_t)full_size);best=full_size;fallback_used=0;
        } else {
            /* AUTO reference policy may choose full-vs-core inside the SAME
             * two-set topology, which is safe for decoders that cache layout. */
            if(full_size<0 && core_size<0) { free(trial);free(full_trial);return -1; }
            if(core_size>=0 && (full_size<0 || core_size<full_size)) {
                memcpy(output,trial,(size_t)core_size);best=core_size;fallback_used=channels-2;
            } else {
                memcpy(output,full_trial,(size_t)full_size);best=full_size;fallback_used=0;
            }
        }
        free(trial);free(full_trial);
        if(used_core) *used_core=fallback_used;
        return best;
    }

    /* Legacy AUTO: compare the one-set and two-set representations separately
     * for every frame. This can change topology mid-stream and is therefore no
     * longer the hdmaenc default, but remains available for diagnostics. */
    if(mode!=HDMA_XLL_REF_CORE) {
        int n=build_hierarchical_candidate(pcm,core_pcm,channels,rate,depth,0,
                                           (HdmaSidePolicy)side_policy,trial,capacity);
        if(n>=0 && (best<0 || n<best)) { memcpy(output,trial,(size_t)n);best=n;fallback_used=0; }
    }
    if(mode!=HDMA_XLL_REF_FULL && core_pcm) {
        int n=build_hierarchical_candidate(pcm,core_pcm,channels,rate,depth,1,
                                           (HdmaSidePolicy)side_policy,trial,capacity);
        if(n==-2 && mode==HDMA_XLL_REF_CORE) { free(trial);free(full_trial);return -2; }
        if(n>=0 && (best<0 || n<best || mode==HDMA_XLL_REF_CORE)) {
            memcpy(output,trial,(size_t)n);best=n;fallback_used=channels-2;
        }
    }
    free(trial);free(full_trial);
    if(used_core) *used_core=fallback_used;
    return best;
}

int hdma_xll_encode_hierarchical_7x_options(const int32_t *pcm,const int32_t *core_pcm,
                                             int channels,int rate,int depth,
                                             uint8_t *output,size_t capacity,int mode,
                                             int *used_core,int ultra,int side_policy)
{
    return hdma_xll_encode_hierarchical_7x_policy_options(pcm,core_pcm,channels,rate,depth,
        output,capacity,mode,used_core,ultra,side_policy,HDMA_XLL_HIER_AUTO);
}
