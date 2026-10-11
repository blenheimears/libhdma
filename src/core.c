/*
 * DCA encoder
 * Copyright (C) 2008-2012 Alexander E. Patrakov
 *               2010 Benjamin Larsson
 *               2011 Xiang Wang
 * Standalone adaptations: Copyright (C) 2026 the contributors to the libhdma project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* Standalone DCA core port. No FFmpeg framework linked or built.
 * Portions adapted from FFmpeg 9.0.2 libavcodec/dcaenc.c, dcaenc.h,
 * dcadata.c, dcahuff.c. See LICENSE-ORIGINS.md and COPYING.LGPL-2.1.
 */
#include "core_internal.h"
#include "core_adpcm.h"

#define MAX_CHANNELS 6
#define DCA_MAX_FRAME_SIZE 16384
#define DCA_HEADER_SIZE 13
#define DCA_LFE_SAMPLES (HDMA_CORE_FRAME_SAMPLES / 64)

#define DCAENC_SUBBANDS 32
#define SUBFRAMES 1
#ifdef HDMA_LONG_FRAME
#define SUBSUBFRAMES 4
#define HDMA_CORE_FRAME_SAMPLES 1024
#else
#define SUBSUBFRAMES 2
#define HDMA_CORE_FRAME_SAMPLES 512
#endif
#define SUBBAND_SAMPLES (SUBFRAMES * SUBSUBFRAMES * 8)
#define AUBANDS 25

#define COS_T(x) (c->cos_table[(x) & 2047])

typedef struct DcaCoreState {
    DcaBitWriter pb;
    int frame_size;
    int frame_bits;
    int fullband_channels;
    int channels;
    int lfe_channel;
    int samplerate_index;
    int bitrate_index;
    int channel_config;
    const int32_t *band_interpolation;
    const int32_t *band_spectrum;
    int lfe_scale_factor;
    softfloat lfe_quant;
    int32_t lfe_peak_cb;
    const int8_t *channel_order_tab;  ///< channel reordering table, lfe and non lfe

    int32_t prediction_mode[MAX_CHANNELS][DCAENC_SUBBANDS];
    int32_t diff_peak_cb[MAX_CHANNELS][DCAENC_SUBBANDS];
    int32_t adpcm_history[MAX_CHANNELS][DCAENC_SUBBANDS][4];
    int adpcm_enabled, adpcm_bands, adpcm_bits, frames_encoded, source_depth;
    int32_t history[MAX_CHANNELS][512]; /* This is a circular buffer */
    int32_t *subband[MAX_CHANNELS][DCAENC_SUBBANDS];
    int32_t quantized[MAX_CHANNELS][DCAENC_SUBBANDS][SUBBAND_SAMPLES];
    int32_t peak_cb[MAX_CHANNELS][DCAENC_SUBBANDS];
    int32_t downsampled_lfe[DCA_LFE_SAMPLES];
    int32_t masking_curve_cb[SUBSUBFRAMES][256];
    int32_t bit_allocation_sel[MAX_CHANNELS];
    int abits[MAX_CHANNELS][DCAENC_SUBBANDS];
    int scale_factor[MAX_CHANNELS][DCAENC_SUBBANDS];
    softfloat quant[MAX_CHANNELS][DCAENC_SUBBANDS];
    int32_t quant_index_sel[MAX_CHANNELS][DCA_CODE_BOOKS];
    int32_t eff_masking_curve_cb[256];
    int32_t band_masking_cb[32];
    int32_t worst_quantization_noise;
    int32_t worst_noise_ever;
    int consumed_bits;

    int32_t cos_table[2048];
    int32_t band_interpolation_tab[2][512];
    int32_t band_spectrum_tab[2][8];
    int32_t auf[9][AUBANDS][256];
    int32_t cb_to_add[256];
    int32_t cb_to_level[2048];
    int32_t lfe_fir_64i[512];
} DcaCoreState;


static uint16_t bitalloc_12_table[DCA_BITALLOC_12_COUNT][12 + 1][2];

static uint16_t bitalloc_table[DCA_NUM_BITALLOC_CODES][2];
static const uint16_t (*bitalloc_tables[DCA_CODE_BOOKS][8])[2];


static double hom(double f)
{
    double f1 = f / 1000;

    return -3.64 * pow(f1, -0.8)
           + 6.8 * exp(-0.6 * (f1 - 3.4) * (f1 - 3.4))
           - 6.0 * exp(-0.15 * (f1 - 8.7) * (f1 - 8.7))
           - 0.0006 * (f1 * f1) * (f1 * f1);
}

static double gammafilter(int i, double f)
{
    double h = (f - fc[i]) / erb[i];

    h = 1 + h * h;
    h = 1 / (h * h);
    return 20 * log10(h);
}

static int subband_bufer_alloc(DcaCoreState *c)
{
    int ch, band;
    int32_t *bufer = calloc(MAX_CHANNELS * DCAENC_SUBBANDS *
                               (SUBBAND_SAMPLES + DCA_ADPCM_COEFFS),
                               sizeof(int32_t));
    if (!bufer)
        return DCA_ERROR(ENOMEM);

    /* we need a place for DCA_ADPCM_COEFF samples from previous frame
     * to calc prediction coefficients for each subband */
    for (ch = 0; ch < MAX_CHANNELS; ch++) {
        for (band = 0; band < DCAENC_SUBBANDS; band++) {
            c->subband[ch][band] = bufer +
                                   ch * DCAENC_SUBBANDS * (SUBBAND_SAMPLES + DCA_ADPCM_COEFFS) +
                                   band * (SUBBAND_SAMPLES + DCA_ADPCM_COEFFS) + DCA_ADPCM_COEFFS;
        }
    }
    return 0;
}

static void subband_bufer_free(DcaCoreState *c)
{
    if (c->subband[0][0]) {
        int32_t *bufer = c->subband[0][0] - DCA_ADPCM_COEFFS;
        free(bufer);
        c->subband[0][0] = NULL;
    }
}

static void create_enc_table(uint16_t dst[][2], unsigned count,
                                     const uint8_t (**src_tablep)[2])
{
    const uint8_t (*src_table)[2] = *src_tablep;
    uint16_t code = 0;

    for (unsigned i = 0; i < count; i++) {
        unsigned dst_idx = src_table[i][0];

        dst[dst_idx][0] = code >> (16 - src_table[i][1]);
        dst[dst_idx][1] = src_table[i][1];

        code += 1 << (16 - src_table[i][1]);
    }
    *src_tablep += count;
}

static void dcaenc_init_static_tables(void)
{
    uint16_t (*bitalloc_dst)[2] = bitalloc_table;
    const uint8_t (*src_table)[2] = dca_vlc_src_tables;

    for (unsigned i = 0; i < DCA_CODE_BOOKS; i++) {
        for (unsigned j = 0; j < dca_quant_index_group_size[i]; j++) {
            create_enc_table(bitalloc_dst, dca_bitalloc_sizes[i],
                             &src_table);
            bitalloc_tables[i][j] = bitalloc_dst - dca_bitalloc_offsets[i];
            bitalloc_dst += dca_bitalloc_sizes[i];
        }
    }

    for (unsigned i = 0; i < DCA_BITALLOC_12_COUNT; i++)
        create_enc_table(&bitalloc_12_table[i][1], 12, &src_table);
}

static int encode_init(DCACodecContext *avctx)
{
    static pthread_once_t init_static_once = PTHREAD_ONCE_INIT;
    DcaCoreState *c = avctx->priv_data;
    DCAChannelLayout layout = avctx->ch_layout;
    int i, j, k, min_frame_bits;
    int ret;

    if ((ret = subband_bufer_alloc(c)) < 0)
        return ret;

    c->fullband_channels = c->channels = layout.nb_channels;
    c->lfe_channel = (c->channels == 6 || layout.stereo_lfe);
    c->band_interpolation = c->band_interpolation_tab[1];
    c->band_spectrum = c->band_spectrum_tab[1];
    c->worst_quantization_noise = -2047;
    c->worst_noise_ever = -2047;
    c->source_depth = 16;


    switch (layout.nb_channels) {
    case 1: /* mono */
        c->channel_config = 0;
        break;
    case 2: /* stereo */
        c->channel_config = 2;
        break;
    case 3: /* 3.0: center + stereo, or 2.1: stereo plus LFE */
        c->channel_config = layout.stereo_lfe ? 2 : 5;
        break;
    case 4: /* 2.2 */
        c->channel_config = 8;
        break;
    case 5: /* 5.0 */
        c->channel_config = 9;
        break;
    case 6: /* 5.1 */
        c->channel_config = 9;
        break;
    default:
        assert(!"impossible channel layout");
    }

    if (c->lfe_channel) {
        c->fullband_channels--;
        c->channel_order_tab = channel_reorder_lfe[c->channel_config];
    } else {
        c->channel_order_tab = channel_reorder_nolfe[c->channel_config];
    }

    for (i = 0; i < MAX_CHANNELS; i++) {
        for (j = 0; j < DCA_CODE_BOOKS; j++) {
            c->quant_index_sel[i][j] = dca_quant_index_group_size[j];
        }
        /* 6 - no Huffman */
        c->bit_allocation_sel[i] = 6;

        for (j = 0; j < DCAENC_SUBBANDS; j++) {
            /* -1 - no ADPCM */
            c->prediction_mode[i][j] = -1;
        }
    }

    for (i = 0; i < 9; i++) {
        if (sample_rates[i] == avctx->sample_rate)
            break;
    }
    if (i == 9)
        return DCA_ERROR(EINVAL);
    c->samplerate_index = i;

    if (avctx->bit_rate < 32000 || avctx->bit_rate > 3840000) {
        dca_log(avctx, DCA_LOG_ERROR, "Bit rate %"PRId64" not supported.", avctx->bit_rate);
        return DCA_ERROR(EINVAL);
    }
    for (i = 0; dca_bit_rates[i] < avctx->bit_rate; i++)
        ;
    c->bitrate_index = i;
    c->frame_bits = DCA_ALIGN((avctx->bit_rate * HDMA_CORE_FRAME_SAMPLES + avctx->sample_rate - 1) / avctx->sample_rate, 32);
    /* DCA's 1536-kbit/s TRANSMISSION RATE code describes the fixed-size
     * 1509-kbit/s payload carried by disc DCA + XLL: 2012 bytes per
     * 512 PCM samples at 48 kHz.  Do not put 2048 payload bytes in a core
     * frame merely because the header's nominal rate is 1536000.  The
     * remaining transport capacity is not part of the core bitstream. */
    if (avctx->bit_rate == 1536000 && avctx->sample_rate == 48000)
        c->frame_bits = 2012 * 8 * (HDMA_CORE_FRAME_SAMPLES / 512);
    min_frame_bits = 132 + (493 + 28 * 32) * c->fullband_channels + c->lfe_channel * 72;
    if (c->frame_bits < min_frame_bits || c->frame_bits > (DCA_MAX_FRAME_SIZE << 3))
        return DCA_ERROR(EINVAL);

    c->frame_size = (c->frame_bits + 7) / 8;

    avctx->frame_size = 32 * SUBBAND_SAMPLES;


    /* Init all tables */
    c->cos_table[0] = 0x7fffffff;
    c->cos_table[512] = 0;
    c->cos_table[1024] = -c->cos_table[0];
    for (i = 1; i < 512; i++) {
        c->cos_table[i]   = (int32_t)(0x7fffffff * cos(M_PI * i / 1024));
        c->cos_table[1024-i] = -c->cos_table[i];
        c->cos_table[1024+i] = -c->cos_table[i];
        c->cos_table[2048-i] = +c->cos_table[i];
    }

    for (i = 0; i < 2048; i++)
        c->cb_to_level[i] = (int32_t)(0x7fffffff * dca_exp10(-0.005 * i));

    for (k = 0; k < 32; k++) {
        for (j = 0; j < 8; j++) {
            c->lfe_fir_64i[64 * j + k] = (int32_t)(((float)UINT64_C(0xffffff800000)) * dca_lfe_fir_64[8 * k + j]);
            c->lfe_fir_64i[64 * (7-j) + (63 - k)] = (int32_t)(((float)UINT64_C(0xffffff800000)) * dca_lfe_fir_64[8 * k + j]);
        }
    }

    for (i = 0; i < 512; i++) {
        c->band_interpolation_tab[0][i] = (int32_t)(0x1000000000ULL * dca_fir_32bands_perfect[i]);
        c->band_interpolation_tab[1][i] = (int32_t)(0x1000000000ULL * dca_fir_32bands_nonperfect[i]);
    }

    for (i = 0; i < 9; i++) {
        for (j = 0; j < AUBANDS; j++) {
            for (k = 0; k < 256; k++) {
                double freq = sample_rates[i] * (k + 0.5) / 512;

                c->auf[i][j][k] = (int32_t)(10 * (hom(freq) + gammafilter(j, freq)));
            }
        }
    }

    for (i = 0; i < 256; i++) {
        double add = 1 + dca_exp10(-0.01 * i);
        c->cb_to_add[i] = (int32_t)(100 * log10(add));
    }
    for (j = 0; j < 8; j++) {
        double accum = 0;
        for (i = 0; i < 512; i++) {
            double reconst = dca_fir_32bands_perfect[i] * ((i & 64) ? (-1) : 1);
            accum += reconst * cos(2 * M_PI * (i + 0.5 - 256) * (j + 0.5) / 512);
        }
        c->band_spectrum_tab[0][j] = (int32_t)(200 * log10(accum));
    }
    for (j = 0; j < 8; j++) {
        double accum = 0;
        for (i = 0; i < 512; i++) {
            double reconst = dca_fir_32bands_nonperfect[i] * ((i & 64) ? (-1) : 1);
            accum += reconst * cos(2 * M_PI * (i + 0.5 - 256) * (j + 0.5) / 512);
        }
        c->band_spectrum_tab[1][j] = (int32_t)(200 * log10(accum));
    }

    pthread_once(&init_static_once, dcaenc_init_static_tables);
    return 0;
}

static int encode_close(DCACodecContext *avctx)
{
    DcaCoreState *c = avctx->priv_data;
    subband_bufer_free(c);

    return 0;
}

static void subband_transform(DcaCoreState *c, const int32_t *input)
{
    int ch, subs, i, k, j;

    for (ch = 0; ch < c->fullband_channels; ch++) {
        /* History is copied because it is also needed for PSY */
        int32_t hist[512];
        int hist_start = 0;
        const int chi = c->channel_order_tab[ch];

        memcpy(hist, &c->history[ch][0], 512 * sizeof(int32_t));

        for (subs = 0; subs < SUBBAND_SAMPLES; subs++) {
            int32_t accum[64];
            int32_t resp;
            int band;

            /* Calculate the convolutions at once */
            memset(accum, 0, 64 * sizeof(int32_t));

            for (k = 0, i = hist_start, j = 0;
                    i < 512; k = (k + 1) & 63, i++, j++)
                accum[k] += mul32(hist[i], c->band_interpolation[j]);
            for (i = 0; i < hist_start; k = (k + 1) & 63, i++, j++)
                accum[k] += mul32(hist[i], c->band_interpolation[j]);

            for (k = 16; k < 32; k++)
                accum[k] = accum[k] - accum[31 - k];
            for (k = 32; k < 48; k++)
                accum[k] = accum[k] + accum[95 - k];

            for (band = 0; band < 32; band++) {
                resp = 0;
                for (i = 16; i < 48; i++) {
                    int s = (2 * band + 1) * (2 * (i + 16) + 1);
                    resp += mul32(accum[i], COS_T(s << 3)) >> 3;
                }

                c->subband[ch][band][subs] = ((band + 1) & 2) ? -resp : resp;
            }

            /* Copy in 32 new samples from input */
            for (i = 0; i < 32; i++)
                hist[i + hist_start] = input[(subs * 32 + i) * c->channels + chi];

            hist_start = (hist_start + 32) & 511;
        }
    }
}

static void lfe_downsample(DcaCoreState *c, const int32_t *input)
{
    /* FIXME: make 128x LFE downsampling possible */
    const int lfech = lfe_index[c->channel_config];
    int i, j, lfes;
    int32_t hist[512];
    int32_t accum;
    int hist_start = 0;

    memcpy(hist, &c->history[c->channels - 1][0], 512 * sizeof(int32_t));

    for (lfes = 0; lfes < DCA_LFE_SAMPLES; lfes++) {
        /* Calculate the convolution */
        accum = 0;

        for (i = hist_start, j = 0; i < 512; i++, j++)
            accum += mul32(hist[i], c->lfe_fir_64i[j]);
        for (i = 0; i < hist_start; i++, j++)
            accum += mul32(hist[i], c->lfe_fir_64i[j]);

        c->downsampled_lfe[lfes] = accum;

        /* Copy in 64 new samples from input */
        for (i = 0; i < 64; i++)
            hist[i + hist_start] = input[(lfes * 64 + i) * c->channels + lfech];

        hist_start = (hist_start + 64) & 511;
    }
}

static uint32_t dca_vlc_calc_alloc_bits(const int values[], uint8_t n, uint8_t sel)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += bitalloc_12_table[sel][values[i]][1];
    return sum;
}

static void dca_vlc_enc_alloc(DcaBitWriter *pb, const int values[],
                              uint8_t n, uint8_t sel)
{
    for (unsigned i = 0; i < n; i++)
        dca_bits_write(pb, bitalloc_12_table[sel][values[i]][1],
                     bitalloc_12_table[sel][values[i]][0]);
}

static uint32_t dca_vlc_calc_quant_bits(const int values[], uint8_t n,
                                        uint8_t sel, uint8_t table)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += bitalloc_tables[table][sel][values[i]][1];
    return sum;
}

static void dca_vlc_enc_quant(DcaBitWriter *pb, const int values[],
                              uint8_t n, uint8_t sel, uint8_t table)
{
    for (unsigned i = 0; i < n; i++)
        dca_bits_write(pb, bitalloc_tables[table][sel][values[i]][1],
                     bitalloc_tables[table][sel][values[i]][0]);
}

static int32_t get_cb(DcaCoreState *c, int32_t in)
{
    int i, res = 0;
    in = DCA_ABS(in);

    for (i = 1024; i > 0; i >>= 1) {
        if (c->cb_to_level[i + res] >= in)
            res += i;
    }
    return -res;
}

static int32_t add_cb(DcaCoreState *c, int32_t a, int32_t b)
{
    if (a < b)
        DCA_SWAP(int32_t, a, b);

    if (a - b >= 256)
        return a;
    return a + c->cb_to_add[a - b];
}

static void calc_power(DcaCoreState *c,
                       const int32_t in[2 * 256], int32_t power[256])
{
    int i;
    DCA_LOCAL_ALIGNED_32(int32_t, data,  [512]);
    DCA_LOCAL_ALIGNED_32(int32_t, coeff, [256]);

    for (i = 0; i < 512; i++)
        data[i] = norm__(mul32(in[i], 0x3fffffff - (COS_T(4 * i + 2) >> 1)), 4);

    standalone_mdct(coeff, data);
    for (i = 0; i < 256; i++) {
        const int32_t cb = get_cb(c, coeff[i]);
        power[i] = add_cb(c, cb, cb);
    }
}

static void adjust_jnd(DcaCoreState *c,
                       const int32_t in[512], int32_t out_cb[256])
{
    int32_t power[256];
    int32_t out_cb_unnorm[256];
    int32_t denom;
    const int32_t ca_cb = -1114;
    const int32_t cs_cb = 928;
    const int samplerate_index = c->samplerate_index;
    int i, j;

    calc_power(c, in, power);

    for (j = 0; j < 256; j++)
        out_cb_unnorm[j] = -2047; /* and can only grow */

    for (i = 0; i < AUBANDS; i++) {
        denom = ca_cb; /* and can only grow */
        for (j = 0; j < 256; j++)
            denom = add_cb(c, denom, power[j] + c->auf[samplerate_index][i][j]);
        for (j = 0; j < 256; j++)
            out_cb_unnorm[j] = add_cb(c, out_cb_unnorm[j],
                                      -denom + c->auf[samplerate_index][i][j]);
    }

    for (j = 0; j < 256; j++)
        out_cb[j] = add_cb(c, out_cb[j], -out_cb_unnorm[j] - ca_cb - cs_cb);
}

typedef void (*walk_band_t)(DcaCoreState *c, int band1, int band2, int f,
                            int32_t spectrum1, int32_t spectrum2, int channel,
                            int32_t * arg);


static void walk_band_low(DcaCoreState *c, int band, int channel,
                          walk_band_t walk, int32_t *arg)
{
    int f;

    if (band == 0) {
        for (f = 0; f < 4; f++)
            walk(c, 0, 0, f, 0, -2047, channel, arg);
    } else {
        for (f = 0; f < 8; f++)
            walk(c, band, band - 1, 8 * band - 4 + f,
                    c->band_spectrum[7 - f], c->band_spectrum[f], channel, arg);
    }
}

static void walk_band_high(DcaCoreState *c, int band, int channel,
                           walk_band_t walk, int32_t *arg)
{
    int f;

    if (band == 31) {
        for (f = 0; f < 4; f++)
            walk(c, 31, 31, 256 - 4 + f, 0, -2047, channel, arg);
    } else {
        for (f = 0; f < 8; f++)
            walk(c, band, band + 1, 8 * band + 4 + f,
                    c->band_spectrum[f], c->band_spectrum[7 - f], channel, arg);
    }
}

static void update_band_masking(DcaCoreState *c, int band1, int band2,
                                int f, int32_t spectrum1, int32_t spectrum2,
                                int channel, int32_t * arg)
{
    (void)band2; (void)spectrum2; (void)channel; (void)arg;
    int32_t value = c->eff_masking_curve_cb[f] - spectrum1;

    if (value < c->band_masking_cb[band1])
        c->band_masking_cb[band1] = value;
}

static void calc_masking(DcaCoreState *c, const int32_t *input)
{
    int i, k, band, ch, ssf;
    int32_t data[512];

    for (i = 0; i < 256; i++)
        for (ssf = 0; ssf < SUBSUBFRAMES; ssf++)
            c->masking_curve_cb[ssf][i] = -2047;

    for (ssf = 0; ssf < SUBSUBFRAMES; ssf++)
        for (ch = 0; ch < c->fullband_channels; ch++) {
            const int chi = c->channel_order_tab[ch];

            /* Windows advance by 256 samples. In 1024 mode the later
             * psychoacoustic windows are entirely within the new frame. */
            for (i=0;i<512;i++) {
                k=128+256*ssf+i;
                data[i]= k<512 ? c->history[ch][k]
                                  : input[(size_t)(k-512)*c->channels+chi];
            }
            adjust_jnd(c, data, c->masking_curve_cb[ssf]);
        }
    for (i = 0; i < 256; i++) {
        int32_t m = 2048;

        for (ssf = 0; ssf < SUBSUBFRAMES; ssf++)
            if (c->masking_curve_cb[ssf][i] < m)
                m = c->masking_curve_cb[ssf][i];
        c->eff_masking_curve_cb[i] = m;
    }

    for (band = 0; band < 32; band++) {
        c->band_masking_cb[band] = 2048;
        walk_band_low(c, band, 0, update_band_masking, NULL);
        walk_band_high(c, band, 0, update_band_masking, NULL);
    }
}

static inline int32_t find_peak(DcaCoreState *c, const int32_t *in, int len)
{
    int sample;
    int32_t m = 0;
    for (sample = 0; sample < len; sample++) {
        int32_t s = abs(in[sample]);
        if (m < s)
            m = s;
    }
    return get_cb(c, m);
}

static void find_peaks(DcaCoreState *c)
{
    int band, ch;

    for (ch = 0; ch < c->fullband_channels; ch++) {
        for (band = 0; band < 32; band++)
            c->peak_cb[ch][band] = find_peak(c, c->subband[ch][band],
                                             SUBBAND_SAMPLES);
    }

    if (c->lfe_channel)
        c->lfe_peak_cb = find_peak(c, c->downsampled_lfe, DCA_LFE_SAMPLES);
}

static int calc_one_scale(DcaCoreState *c, int32_t peak_cb, int abits,
                          softfloat *quant)
{
    int32_t peak;
    int our_nscale, try_remove;
    softfloat our_quant;

    assert(peak_cb <= 0);
    assert(peak_cb >= -2047);

    our_nscale = 127;
    peak = c->cb_to_level[-peak_cb];

    for (try_remove = 64; try_remove > 0; try_remove >>= 1) {
        if (scalefactor_inv[our_nscale - try_remove].e + stepsize_inv[abits].e <= 17)
            continue;
        our_quant.m = mul32(scalefactor_inv[our_nscale - try_remove].m, stepsize_inv[abits].m);
        our_quant.e = scalefactor_inv[our_nscale - try_remove].e + stepsize_inv[abits].e - 17;
        if ((int32_t)((dca_quant_levels[abits] - 1) / 2) < quantize_value(peak, our_quant))
            continue;
        our_nscale -= try_remove;
    }

    if (our_nscale >= 125)
        our_nscale = 124;

    quant->m = mul32(scalefactor_inv[our_nscale].m, stepsize_inv[abits].m);
    quant->e = scalefactor_inv[our_nscale].e + stepsize_inv[abits].e - 17;
    assert((int32_t)((dca_quant_levels[abits] - 1) / 2) >= quantize_value(peak, *quant));

    return our_nscale;
}

/* Defined by the standalone decoder-exact synthesis module below. */
static int32_t hdma_dequantize(int32_t input,unsigned abits,unsigned scale_index);

static void analyze_adpcm(DcaCoreState *c) {
    c->adpcm_bands=0;c->adpcm_bits=0;
    for(int ch=0;ch<c->fullband_channels;ch++)for(int band=0;band<32;band++) {
        c->prediction_mode[ch][band]=-1;
        if(!c->adpcm_enabled)continue;
        int32_t *samples=c->subband[ch][band];
        for(int h=0;h<4;h++)
            samples[h-4]=c->adpcm_history[ch][band][h]*128;
        int32_t diff[SUBBAND_SAMPLES];
        int id=hdma_adpcm_select(samples-4,SUBBAND_SAMPLES,diff);
        if(id<0)continue;
        c->prediction_mode[ch][band]=id;
        c->diff_peak_cb[ch][band]=find_peak(c,diff,SUBBAND_SAMPLES);
        c->adpcm_bands++;c->adpcm_bits+=12;
    }
}
/* The allocator retries quantization repeatedly. Never advance prediction
 * history here: every pass starts with the exact previous frame history. */
static void quantize_adpcm(DcaCoreState *c) {
    for(int ch=0;ch<c->fullband_channels;ch++)for(int band=0;band<32;band++) {
        int id=c->prediction_mode[ch][band];
        if(id<0)continue;
        int abits=c->abits[ch][band];
        int32_t hist[4];
        memcpy(hist,c->adpcm_history[ch][band],sizeof(hist));
        int32_t peak=c->cb_to_level[-c->diff_peak_cb[ch][band]];
        for(int i=0;i<SUBBAND_SAMPLES;i++) {
            int32_t pred=hdma_adpcm_predict(id,hist);
            int64_t delta=(int64_t)c->subband[ch][band][i]-(int64_t)pred*128;
            if(delta>peak)delta=peak;
            if(delta<-(int64_t)peak)delta=-(int64_t)peak;
            int32_t q=abits ? quantize_value((int32_t)delta,c->quant[ch][band]) : 0;
            c->quantized[ch][band][i]=q;
            hdma_adpcm_reconstruct(id,abits ? hdma_dequantize(q,(unsigned)abits,
                (unsigned)c->scale_factor[ch][band]) : 0,hist);
        }
    }
}
static void update_adpcm_history(DcaCoreState *c) {
    if(!c->adpcm_enabled)return;
    for(int ch=0;ch<c->fullband_channels;ch++)for(int band=0;band<32;band++) {
        int32_t hist[4];
        memcpy(hist,c->adpcm_history[ch][band],sizeof(hist));
        int abits=c->abits[ch][band];
        for(int i=0;i<SUBBAND_SAMPLES;i++) {
            int32_t q=c->quantized[ch][band][i];
            int32_t dequant=abits ? hdma_dequantize(q,(unsigned)abits,
                (unsigned)c->scale_factor[ch][band]) : 0;
            hdma_adpcm_reconstruct(c->prediction_mode[ch][band],dequant,hist);
        }
        memcpy(c->adpcm_history[ch][band],hist,sizeof(hist));
    }
}
static void quantize_pcm(DcaCoreState *c)
{
    int sample, band, ch;

    for (ch = 0; ch < c->fullband_channels; ch++) {
        for (band = 0; band < 32; band++) {
            if (c->prediction_mode[ch][band] == -1) {
                for (sample = 0; sample < SUBBAND_SAMPLES; sample++) {
                    int32_t val = quantize_value(c->subband[ch][band][sample],
                                                 c->quant[ch][band]);
                    c->quantized[ch][band][sample] = val;
                }
            }
        }
    }
}

static void accumulate_huff_bit_consumption(int abits, int32_t *quantized,
                                            uint32_t *result)
{
    uint8_t sel, id = abits - 1;
    for (sel = 0; sel < dca_quant_index_group_size[id]; sel++)
        result[sel] += dca_vlc_calc_quant_bits(quantized, SUBBAND_SAMPLES,
                                               sel, id);
}

static uint32_t set_best_code(uint32_t vlc_bits[DCA_CODE_BOOKS][7],
                              uint32_t clc_bits[DCA_CODE_BOOKS],
                              int32_t res[DCA_CODE_BOOKS])
{
    uint8_t i, sel;
    uint32_t best_sel_bits[DCA_CODE_BOOKS];
    int32_t best_sel_id[DCA_CODE_BOOKS];
    uint32_t t, bits = 0;

    for (i = 0; i < DCA_CODE_BOOKS; i++) {

        assert(!((!!vlc_bits[i][0]) ^ (!!clc_bits[i])));
        if (vlc_bits[i][0] == 0) {
            /* do not transmit adjustment index for empty codebooks */
            res[i] = dca_quant_index_group_size[i];
            /* and skip it */
            continue;
        }

        best_sel_bits[i] = vlc_bits[i][0];
        best_sel_id[i] = 0;
        for (sel = 0; sel < dca_quant_index_group_size[i]; sel++) {
            if (best_sel_bits[i] > vlc_bits[i][sel] && vlc_bits[i][sel]) {
                best_sel_bits[i] = vlc_bits[i][sel];
                best_sel_id[i] = sel;
            }
        }

        /* 2 bits to transmit scale factor adjustment index */
        t = best_sel_bits[i] + 2;
        if (t < clc_bits[i]) {
            res[i] = best_sel_id[i];
            bits += t;
        } else {
            res[i] = dca_quant_index_group_size[i];
            bits += clc_bits[i];
        }
    }
    return bits;
}

static uint32_t set_best_abits_code(int abits[DCAENC_SUBBANDS], int bands,
                                    int32_t *res)
{
    uint8_t i;
    uint32_t t;
    int32_t best_sel = 6;
    int32_t best_bits = bands * 5;

    /* Check do we have subband which cannot be encoded by Huffman tables */
    for (i = 0; i < bands; i++) {
        if (abits[i] > 12 || abits[i] == 0) {
            *res = best_sel;
            return best_bits;
        }
    }

    for (i = 0; i < DCA_BITALLOC_12_COUNT; i++) {
        t = dca_vlc_calc_alloc_bits(abits, bands, i);
        if ((int32_t)t < best_bits) {
            best_bits = t;
            best_sel = i;
        }
    }

    *res = best_sel;
    return best_bits;
}

#define USED_1ABITS 1
#define USED_26ABITS 4
static const int snr_fudge = 128;
static int init_quantization_noise(DcaCoreState *c, int noise, int forbid_zero)
{
    int ch, band, ret = USED_26ABITS | USED_1ABITS;
    uint32_t huff_bit_count_accum[MAX_CHANNELS][DCA_CODE_BOOKS][7];
    uint32_t clc_bit_count_accum[MAX_CHANNELS][DCA_CODE_BOOKS];
    uint32_t bits_counter = 0;

    c->consumed_bits = 132 + 333 * c->fullband_channels + c->adpcm_bits;
    if (c->lfe_channel)
        c->consumed_bits += 72;

    /* attempt to guess the bit distribution based on the previous frame */
    for (ch = 0; ch < c->fullband_channels; ch++) {
        for (band = 0; band < 32; band++) {
            int snr_cb = c->peak_cb[ch][band] - c->band_masking_cb[band] - noise;

            if (snr_cb >= 1312) {
                c->abits[ch][band] = 26;
                ret &= ~USED_1ABITS;
            } else if (snr_cb >= 222) {
                c->abits[ch][band] = 8 + mul32(snr_cb - 222, 69000000);
                ret &= ~(USED_26ABITS | USED_1ABITS);
            } else if (snr_cb >= 0) {
                c->abits[ch][band] = 2 + mul32(snr_cb, 106000000);
                ret &= ~(USED_26ABITS | USED_1ABITS);
            } else if (forbid_zero || snr_cb >= -140) {
                c->abits[ch][band] = 1;
                ret &= ~USED_26ABITS;
            } else {
                c->abits[ch][band] = 0;
                ret &= ~(USED_26ABITS | USED_1ABITS);
            }
        }
        c->consumed_bits += set_best_abits_code(c->abits[ch], 32,
                                                &c->bit_allocation_sel[ch]);
    }

    /* Recalc scale_factor each time to get bits consumption in case of Huffman coding.
       It is suboptimal solution */
    /* TODO: May be cache scaled values */
    for (ch = 0; ch < c->fullband_channels; ch++) {
        for (band = 0; band < 32; band++) {
            if (c->abits[ch][band] == 0 && c->prediction_mode[ch][band]>=0) {
                c->scale_factor[ch][band]=0;
            } else {
                int32_t peak=c->prediction_mode[ch][band]>=0 ?
                    c->diff_peak_cb[ch][band] : c->peak_cb[ch][band];
                c->scale_factor[ch][band] = calc_one_scale(c,peak,
                                                           c->abits[ch][band],
                                                           &c->quant[ch][band]);
            }
        }
    }
    quantize_pcm(c);
    if(c->adpcm_enabled)quantize_adpcm(c);

    memset(huff_bit_count_accum, 0, MAX_CHANNELS * DCA_CODE_BOOKS * 7 * sizeof(uint32_t));
    memset(clc_bit_count_accum, 0, MAX_CHANNELS * DCA_CODE_BOOKS * sizeof(uint32_t));
    for (ch = 0; ch < c->fullband_channels; ch++) {
        for (band = 0; band < 32; band++) {
            if (c->abits[ch][band] && c->abits[ch][band] <= DCA_CODE_BOOKS) {
                accumulate_huff_bit_consumption(c->abits[ch][band],
                                                c->quantized[ch][band],
                                                huff_bit_count_accum[ch][c->abits[ch][band] - 1]);
                clc_bit_count_accum[ch][c->abits[ch][band] - 1] += bit_consumption[c->abits[ch][band]] * (SUBBAND_SAMPLES / 16);
            } else {
                bits_counter += bit_consumption[c->abits[ch][band]] * (SUBBAND_SAMPLES / 16);
            }
        }
    }

    for (ch = 0; ch < c->fullband_channels; ch++) {
        bits_counter += set_best_code(huff_bit_count_accum[ch],
                                      clc_bit_count_accum[ch],
                                      c->quant_index_sel[ch]);
    }

    c->consumed_bits += bits_counter;

    return ret;
}

static void assign_bits(DcaCoreState *c)
{
    /* Find the bounds where the binary search should work */
    int low, high, down;
    /* Account for integer bit-cost estimates and per-subsubframe framing.
     * Extended frames contain twice the quantized symbols and may differ
     * from the estimated VLC payload by several dozen bits. */
    const int target_bits = c->frame_bits - 16 * SUBSUBFRAMES -
                            (HDMA_CORE_FRAME_SAMPLES == 1024 ? 256 : 0);
    int used_abits = 0;
    int forbid_zero = 1;
restart:
    init_quantization_noise(c, c->worst_quantization_noise, forbid_zero);
    low = high = c->worst_quantization_noise;
    if (c->consumed_bits > target_bits) {
        while (c->consumed_bits > target_bits) {
            if (used_abits == USED_1ABITS && forbid_zero) {
                forbid_zero = 0;
                goto restart;
            }
            low = high;
            high += snr_fudge;
            used_abits = init_quantization_noise(c, high, forbid_zero);
        }
    } else {
        while (c->consumed_bits <= target_bits) {
            high = low;
            if (used_abits == USED_26ABITS)
                goto out; /* The requested bitrate is too high, pad with zeros */
            low -= snr_fudge;
            used_abits = init_quantization_noise(c, low, forbid_zero);
        }
    }

    /* Now do a binary search between low and high to see what fits */
    for (down = snr_fudge >> 1; down; down >>= 1) {
        init_quantization_noise(c, high - down, forbid_zero);
        if (c->consumed_bits <= target_bits)
            high -= down;
    }
    init_quantization_noise(c, high, forbid_zero);
out:
    c->worst_quantization_noise = high;
    if (high > c->worst_noise_ever)
        c->worst_noise_ever = high;
}

static void shift_history(DcaCoreState *c, const int32_t *input)
{
    int k, ch;

    /* The reorder table contains full-band channels only.  Its next entry
     * is -1 when an LFE channel exists, so copy LFE separately to the last
     * history plane instead of indexing before the input buffer. */
    for (k = 0; k < 512; k++) {
        for (ch = 0; ch < c->fullband_channels; ch++) {
            const int chi = c->channel_order_tab[ch];
            c->history[ch][k] = input[(k + HDMA_CORE_FRAME_SAMPLES - 512) * c->channels + chi];
        }
        if (c->lfe_channel)
            c->history[c->channels - 1][k] =
                input[(k + HDMA_CORE_FRAME_SAMPLES - 512) * c->channels + lfe_index[c->channel_config]];
    }
}

static void calc_lfe_scales(DcaCoreState *c)
{
    if (c->lfe_channel)
        c->lfe_scale_factor = calc_one_scale(c, c->lfe_peak_cb, 11, &c->lfe_quant);
}

static void put_frame_header(DcaCoreState *c)
{
    /* SYNC */
    dca_bits_write(&c->pb, 16, 0x7ffe);
    dca_bits_write(&c->pb, 16, 0x8001);

    /* Frame type: normal */
    dca_bits_write(&c->pb, 1, 1);

    /* Deficit sample count: none */
    dca_bits_write(&c->pb, 5, 31);

    /* CRC is not present */
    dca_bits_write(&c->pb, 1, 0);

    /* Number of PCM sample blocks */
    dca_bits_write(&c->pb, 7, SUBBAND_SAMPLES - 1);

    /* Primary frame byte size */
    dca_bits_write(&c->pb, 14, c->frame_size - 1);

    /* Audio channel arrangement */
    dca_bits_write(&c->pb, 6, c->channel_config);

    /* Core audio sampling frequency */
    dca_bits_write(&c->pb, 4, bitstream_sfreq[c->samplerate_index]);

    /* Transmission bit rate */
    dca_bits_write(&c->pb, 5, c->bitrate_index);

    /* Embedded down mix: disabled */
    dca_bits_write(&c->pb, 1, 0);

    /* Embedded dynamic range flag: not present */
    dca_bits_write(&c->pb, 1, 0);

    /* Embedded time stamp flag: not present */
    dca_bits_write(&c->pb, 1, 0);

    /* Auxiliary data flag: not present */
    dca_bits_write(&c->pb, 1, 0);

    /* HDCD source: no */
    dca_bits_write(&c->pb, 1, 0);

    /* Extension audio ID: N/A */
    dca_bits_write(&c->pb, 3, 0);

    /* Extended audio data: not present */
    dca_bits_write(&c->pb, 1, 0);

    /* Match the official encoder: auxiliary sync words are enabled for
     * sub-subframes. Several hardware decoders key their core/EXSS cadence
     * from this flag even though software decoders tolerate it being clear. */
    dca_bits_write(&c->pb, 1, 1);

    /* Low frequency effects flag: not present or 64x subsampling */
    dca_bits_write(&c->pb, 2, c->lfe_channel ? 2 : 0);

    /* Predictor history switch flag: on */
    dca_bits_write(&c->pb, 1, 1);

    /* No CRC */
    /* Multirate interpolator switch: non-perfect reconstruction */
    dca_bits_write(&c->pb, 1, 0);

    /* Encoder software revision: 7 */
    dca_bits_write(&c->pb, 4, 7);

    /* Match the official encoder's copy-history metadata. */
    dca_bits_write(&c->pb, 2, 1);

    /* The official 24-bit core uses PCM-resolution code 6. Keep the proven
     * 16-bit code 0 until a matching official 16-bit sample establishes a
     * different convention. */
    dca_bits_write(&c->pb, 3, c->source_depth == 24 ? 6 : 0);

    /* Front sum/difference coding: no */
    dca_bits_write(&c->pb, 1, 0);

    /* Surrounds sum/difference coding: no */
    dca_bits_write(&c->pb, 1, 0);

    /* Dialog normalization: 0 dB */
    dca_bits_write(&c->pb, 4, 0);
}

static void put_primary_audio_header(DcaCoreState *c)
{
    int ch, i;
    /* Number of subframes */
    dca_bits_write(&c->pb, 4, SUBFRAMES - 1);

    /* Number of primary audio channels */
    dca_bits_write(&c->pb, 3, c->fullband_channels - 1);

    /* Subband activity count */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 5, DCAENC_SUBBANDS - 2);

    /* High frequency VQ start subband */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 5, DCAENC_SUBBANDS - 1);

    /* Joint intensity coding index: 0, 0 */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 3, 0);

    /* Transient mode codebook: A4, A4 (arbitrary) */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 2, 0);

    /* Scale factor code book: 7 bit linear, 7-bit sqrt table (for each channel) */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 3, 6);

    /* Bit allocation quantizer select: linear 5-bit */
    for (ch = 0; ch < c->fullband_channels; ch++)
        dca_bits_write(&c->pb, 3, c->bit_allocation_sel[ch]);

    /* Quantization index codebook select */
    for (i = 0; i < DCA_CODE_BOOKS; i++)
        for (ch = 0; ch < c->fullband_channels; ch++)
            dca_bits_write(&c->pb, dca_quant_index_sel_nbits[i], c->quant_index_sel[ch][i]);

    /* Scale factor adjustment index: transmitted in case of Huffman coding */
    for (i = 0; i < DCA_CODE_BOOKS; i++)
        for (ch = 0; ch < c->fullband_channels; ch++)
            if (c->quant_index_sel[ch][i] < dca_quant_index_group_size[i])
                dca_bits_write(&c->pb, 2, 0);

    /* Audio header CRC check word: not transmitted */
}

static void put_subframe_samples(DcaCoreState *c, int ss, int band, int ch)
{
    int i, j, sum, bits, sel;
    if (c->abits[ch][band] <= DCA_CODE_BOOKS) {
        assert(c->abits[ch][band] > 0);
        sel = c->quant_index_sel[ch][c->abits[ch][band] - 1];
        // Huffman codes
        if (sel < dca_quant_index_group_size[c->abits[ch][band] - 1]) {
            dca_vlc_enc_quant(&c->pb, &c->quantized[ch][band][ss * 8], 8,
                              sel, c->abits[ch][band] - 1);
            return;
        }

        // Block codes
        if (c->abits[ch][band] <= 7) {
            for (i = 0; i < 8; i += 4) {
                sum = 0;
                for (j = 3; j >= 0; j--) {
                    sum *= dca_quant_levels[c->abits[ch][band]];
                    sum += c->quantized[ch][band][ss * 8 + i + j];
                    sum += (dca_quant_levels[c->abits[ch][band]] - 1) / 2;
                }
                dca_bits_write(&c->pb, bit_consumption[c->abits[ch][band]] / 4, sum);
            }
            return;
        }
    }

    for (i = 0; i < 8; i++) {
        bits = bit_consumption[c->abits[ch][band]] / 16;
        dca_bits_signed(&c->pb, bits, c->quantized[ch][band][ss * 8 + i]);
    }
}

static void put_subframe(DcaCoreState *c, int subframe)
{
    (void)subframe;
    int i, band, ss, ch;

    /* Subsubframes count */
    dca_bits_write(&c->pb, 2, SUBSUBFRAMES -1);

    /* Partial subsubframe sample count: dummy */
    dca_bits_write(&c->pb, 3, 0);

    /* Prediction mode: no ADPCM, in each channel and subband */
    for (ch = 0; ch < c->fullband_channels; ch++)
        for (band = 0; band < DCAENC_SUBBANDS; band++)
            dca_bits_write(&c->pb, 1, !(c->prediction_mode[ch][band] == -1));

    /* Prediction VQ address */
    for (ch = 0; ch < c->fullband_channels; ch++)
        for (band = 0; band < DCAENC_SUBBANDS; band++)
            if (c->prediction_mode[ch][band] >= 0)
                dca_bits_write(&c->pb, 12, c->prediction_mode[ch][band]);

    /* Bit allocation index */
    for (ch = 0; ch < c->fullband_channels; ch++) {
        if (c->bit_allocation_sel[ch] == 6) {
            for (band = 0; band < DCAENC_SUBBANDS; band++) {
                dca_bits_write(&c->pb, 5, c->abits[ch][band]);
            }
        } else {
            dca_vlc_enc_alloc(&c->pb, c->abits[ch], DCAENC_SUBBANDS,
                              c->bit_allocation_sel[ch]);
        }
    }

    if (SUBSUBFRAMES > 1) {
        /* Transition mode: none for each channel and subband */
        for (ch = 0; ch < c->fullband_channels; ch++)
            for (band = 0; band < DCAENC_SUBBANDS; band++)
                if (c->abits[ch][band])
                    dca_bits_write(&c->pb, 1, 0); /* codebook A4 */
    }

    /* Scale factors */
    for (ch = 0; ch < c->fullband_channels; ch++)
        for (band = 0; band < DCAENC_SUBBANDS; band++)
            if (c->abits[ch][band])
                dca_bits_write(&c->pb, 7, c->scale_factor[ch][band]);

    /* Joint subband scale factor codebook select: not transmitted */
    /* Scale factors for joint subband coding: not transmitted */
    /* Stereo down-mix coefficients: not transmitted */
    /* Dynamic range coefficient: not transmitted */
    /* Stde information CRC check word: not transmitted */
    /* VQ encoded high frequency subbands: not transmitted */

    /* LFE data: 8 samples and scalefactor */
    if (c->lfe_channel) {
        for (i = 0; i < DCA_LFE_SAMPLES; i++)
            dca_bits_write(&c->pb, 8, quantize_value(c->downsampled_lfe[i], c->lfe_quant) & 0xff);
        dca_bits_write(&c->pb, 8, c->lfe_scale_factor);
    }

    /* Audio data. With the header's sync-insertion flag set, the reference
     * encoder terminates every sub-subframe with DSYNC, not merely the whole
     * subframe. */
    for (ss = 0; ss < SUBSUBFRAMES ; ss++) {
        for (ch = 0; ch < c->fullband_channels; ch++)
            for (band = 0; band < DCAENC_SUBBANDS; band++)
                if (c->abits[ch][band])
                    put_subframe_samples(c, ss, band, ch);
        dca_bits_write(&c->pb, 16, 0xffff);
    }
}

static int encode_frame(DCACodecContext *avctx, DCAPacket *avpkt,
                        const DCAFrame *frame, int *got_packet_ptr)
{
    DcaCoreState *c = avctx->priv_data;
    const int32_t *samples;
    int ret, i;

    if ((ret = dca_get_encode_buffer(avctx, avpkt, c->frame_size, 0)) < 0)
        return ret;

    samples = (const int32_t *)frame->data[0];

    subband_transform(c, samples);
    if (c->lfe_channel)
        lfe_downsample(c, samples);

    calc_masking(c, samples);
    find_peaks(c);
    analyze_adpcm(c);
    assign_bits(c);
    calc_lfe_scales(c);
    shift_history(c, samples);
    update_adpcm_history(c);

    dca_bits_init(&c->pb, avpkt->data, avpkt->size);
    put_frame_header(c);
    put_primary_audio_header(c);
    for (i = 0; i < SUBFRAMES; i++)
        put_subframe(c, i);

    if (c->pb.overflow || c->pb.bits > c->pb.capacity * 8) return DCA_ERROR(EINVAL);
    dca_bits_flush(&c->pb);
    memset(dca_bits_ptr(&c->pb), 0, dca_bits_bytes_left(&c->pb, 0));

    *got_packet_ptr = 1;
    c->frames_encoded++;
    return 0;
}

/* Project-owned public encoder API: FFmpeg codec registry is not used. */
#ifdef HDMA_LONG_FRAME
#define dca_core_create dca_core_create_1024
#define dca_core_create_layout dca_core_create_layout_1024
#define dca_core_frame_bytes dca_core_frame_bytes_1024
#define dca_core_encode dca_core_encode_1024
#define dca_core_reconstruct dca_core_reconstruct_1024
#define dca_core_destroy dca_core_destroy_1024
#define dca_core_set_adpcm dca_core_set_adpcm_1024
#define dca_core_set_source_depth dca_core_set_source_depth_1024
#define dca_core_adpcm_bands dca_core_adpcm_bands_1024
#endif
/* Fixed-point core PCM synthesis state is retained across every frame. The
 * encoder's quantized frequency samples are reused before bitstream packing;
 * no Huffman or block-code entropy decoding is performed. */
typedef struct {
    int32_t hist[6][1024];
    int32_t accum[6][32];
    int offset[6];
    int32_t lfe_history[8];
    int32_t adpcm_history[6][32][4];
} CoreReference;
struct DcaCore { DcaCoreState state; DCACodecContext avctx; CoreReference reference; };
#include "core_reference_impl.h"
int dca_core_create_layout(DcaCore **out, int channels, int rate, int bitrate, int stereo_lfe) {
    if ((stereo_lfe != 0 && stereo_lfe != 1) || (stereo_lfe && channels != 3)) return -1;
    DcaCore *codec;
    if (!out || (channels != 1 && channels != 2 && channels != 3 && channels != 4 && channels != 5 && channels != 6)
        || rate <= 0 || bitrate <= 0) return -1;
    *out = NULL;
    int valid_rate=0, valid_bitrate=0;
    for(int i=0;i<9;i++)if(rate==sample_rates[i])valid_rate=1;
    for(int i=0;i<29;i++)if(bitrate==(int)dca_bit_rates[i])valid_bitrate=1;
    if(!valid_rate || !valid_bitrate)return -1;
    codec = calloc(1, sizeof(*codec));
    if (!codec) return -1;
    codec->avctx.priv_data = &codec->state;
    codec->avctx.ch_layout.nb_channels = channels;
    codec->avctx.ch_layout.stereo_lfe = stereo_lfe;
    codec->avctx.sample_rate = rate;
    codec->avctx.bit_rate = bitrate;
    if (encode_init(&codec->avctx) != 0) {
        encode_close(&codec->avctx); free(codec); return -1;
    }
    *out = codec;
    return 0;
}
int dca_core_create(DcaCore **out, int channels, int rate, int bitrate) {
    return dca_core_create_layout(out,channels,rate,bitrate,0);
}
int dca_core_set_adpcm(DcaCore *codec,int enabled) {
    if(!codec || (enabled!=0 && enabled!=1) || codec->state.frames_encoded)return -1;
    codec->state.adpcm_enabled=enabled;
    return 0;
}
int dca_core_set_source_depth(DcaCore *codec,int depth) {
    if(!codec || (depth!=16 && depth!=24) || codec->state.frames_encoded)return -1;
    codec->state.source_depth=depth;
    return 0;
}
int dca_core_adpcm_bands(const DcaCore *codec) {
    return codec ? codec->state.adpcm_bands : -1;
}
int dca_core_frame_bytes(const DcaCore *codec) { return codec ? codec->state.frame_size : 0; }
int dca_core_encode(DcaCore *codec, const int32_t *pcm, uint8_t *dst, size_t capacity) {
    if (!codec || !pcm || !dst || capacity < (size_t)codec->state.frame_size) return -1;
    DCAFrame frame = {{(const uint8_t *)pcm}};
    DCAPacket pkt = { dst, codec->state.frame_size };
    int got = 0;
    if (encode_frame(&codec->avctx, &pkt, &frame, &got) != 0 || !got) return -1;
    return pkt.size;
}
/* This is the same synthesis that an XLL-capable decoder uses for core
 * reconstruction, fed directly from the quantized encoder samples. Output
 * is in input channel order, signed 24-bit values in int32_t containers. */
int dca_core_reconstruct(DcaCore *codec, int32_t *dst, size_t samples)
{
    if (!codec || !dst || samples < (size_t)codec->state.channels * HDMA_CORE_FRAME_SAMPLES) return -1;
    DcaCoreState *c=&codec->state;
    CoreReference *r=&codec->reference;
    int32_t decoded[32];
    for (int ch=0;ch<c->fullband_channels;ch++) {
        int input_ch=c->channel_order_tab[ch];
        if (input_ch<0 || input_ch>=c->channels) return -1;
        for (int block=0;block<SUBBAND_SAMPLES;block++) {
            int32_t bands[32];
            for (int band=0;band<32;band++) {
                unsigned abits=(unsigned)c->abits[ch][band];
                int32_t residual=abits ? hdma_dequantize(
                    c->quantized[ch][band][block],abits,
                    (unsigned)c->scale_factor[ch][band]) : 0;
                bands[band]=hdma_adpcm_reconstruct(c->prediction_mode[ch][band],
                    residual,r->adpcm_history[ch][band]);
            }
            hdma_synth_filter_fixed(r->hist[ch], &r->offset[ch],r->accum[ch],
                hdma_fir_nonperfect,decoded,bands);
            for (int n=0;n<32;n++)
                dst[((size_t)block*32U+(unsigned)n)*c->channels+(unsigned)input_ch]=decoded[n];
        }
    }
    if (c->lfe_channel) {
        int input_ch=lfe_index[c->channel_config];
        int32_t samples_lfe[DCA_LFE_SAMPLES + 8];
        memcpy(samples_lfe,r->lfe_history,sizeof(r->lfe_history));
        for (int n=0;n<DCA_LFE_SAMPLES;n++) {
            int q=quantize_value(c->downsampled_lfe[n],c->lfe_quant);
            samples_lfe[8+n]=hdma_lfe_dequantize((int8_t)(uint8_t)q,
                (unsigned)c->lfe_scale_factor);
        }
        for (int block=0;block<DCA_LFE_SAMPLES;block++) {
            const int32_t *current=samples_lfe+8+block;
            for (int n=0;n<32;n++) {
                int64_t a=0,b=0;
                for (int k=0;k<8;k++) {
                    a+=(int64_t)hdma_lfe_fir[n*8+k]*current[-k];
                    b+=(int64_t)hdma_lfe_fir[255-n*8-k]*current[-k];
                }
                dst[((size_t)block*64U+(unsigned)n)*c->channels+(unsigned)input_ch]=clip23(norm23(a));
                dst[((size_t)block*64U+(unsigned)n+32U)*c->channels+(unsigned)input_ch]=clip23(norm23(b));
            }
        }
        memcpy(r->lfe_history,samples_lfe+DCA_LFE_SAMPLES,sizeof(r->lfe_history));
    }
    return 0;
}
void dca_core_destroy(DcaCore *codec) {
    if (codec) { encode_close(&codec->avctx); free(codec); }
}
