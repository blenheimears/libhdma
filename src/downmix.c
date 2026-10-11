/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Reversible source isolation: downmix never modifies lossless source PCM.
 */
#include "downmix.h"
#include <stddef.h>
#include <limits.h>

/* Standard 7.x -> 5.x compatibility downmix.
 *
 * The supplied known-good 7.1 DCA + XLL reference uses the DCA hierarchy's
 * -3 dB scale (D.11 code 473, 23170/32768) on every primary 5.x channel.
 * Surround energy is routed ONLY to the corresponding surround: Lss+Lsr ->
 * Ls and Rss+Rsr -> Rs. Nothing is mixed into FL/FR.  Saturation is required
 * for same-polarity full-scale surround pairs and matches the purpose of the
 * reference encoder's downmix-saturation check.  Explicit FRONT/REAR/MUTE
 * values are retained only as legacy diagnostic overrides. */
static int32_t blend_legacy(int32_t main, int32_t side)
{
    return (int32_t)(((int64_t)main * 3 + (int64_t)side) / 4);
}

static int32_t clip_i32(int64_t value)
{
    if(value>INT32_MAX) return INT32_MAX;
    if(value<INT32_MIN) return INT32_MIN;
    return (int32_t)value;
}

static int32_t mul_q15_ffmpeg(int32_t sample,int32_t coeff)
{
    /* Match FFmpeg dcamath.h norm15/mul15, including negative rounding. */
    return (int32_t)(((int64_t)sample*coeff + 16384) >> 15);
}

static int32_t standard_scale(int32_t sample)
{
    return mul_q15_ffmpeg(sample,23170);
}

static int32_t standard_surround_fold(int32_t rear, int32_t side)
{
    /* The hierarchy's effective undo coefficient is 32767/32768, not exact
     * unity: code 473 * inverse(code 473) rounds to 32767.  Build the same
     * primary value the decoder will later undo, then apply its -3 dB scale. */
    int64_t primary=(int64_t)side + mul_q15_ffmpeg(rear,32767);
    int64_t product=primary*23170;
    return clip_i32((product+16384) >> 15);
}

int hdma_downmix_core_policy(const int32_t *original, int channels,
                             int32_t *core, HdmaSidePolicy policy)
{
    if (!original || !core || (channels != 7 && channels != 8) ||
        policy < HDMA_SIDE_BOTH || policy > HDMA_SIDE_MUTE) return -1;
    int lfe = channels == 8;
    int core_channels = channels - 2;
    for (int n = 0; n < 512; ++n) {
        size_t source = (size_t)n * (size_t)channels;
        size_t target = (size_t)n * (size_t)core_channels;
        int back = lfe ? 4 : 3;
        int side = lfe ? 6 : 5;

        core[target] = policy==HDMA_SIDE_BOTH ? standard_scale(original[source]) : original[source];
        core[target + 1] = policy==HDMA_SIDE_BOTH ? standard_scale(original[source + 1]) : original[source + 1];
        core[target + 2] = policy==HDMA_SIDE_BOTH ? standard_scale(original[source + 2]) : original[source + 2];
        if (lfe) core[target + 3] = policy==HDMA_SIDE_BOTH ? standard_scale(original[source + 3]) : original[source + 3];

        if(policy==HDMA_SIDE_BOTH) {
            core[target + core_channels - 2] = standard_surround_fold(
                original[source + back], original[source + side]);
            core[target + core_channels - 1] = standard_surround_fold(
                original[source + back + 1], original[source + side + 1]);
        } else if(policy==HDMA_SIDE_FRONT) {
            /* Legacy diagnostic override: side pair feeds front only. */
            core[target] = blend_legacy(original[source], original[source + side]);
            core[target + 1] = blend_legacy(original[source + 1], original[source + side + 1]);
            core[target + core_channels - 2] = original[source + back];
            core[target + core_channels - 1] = original[source + back + 1];
        } else if(policy==HDMA_SIDE_REAR) {
            /* Legacy diagnostic override: old 3/4 rear + 1/4 side fold. */
            core[target + core_channels - 2] = blend_legacy(
                original[source + back], original[source + side]);
            core[target + core_channels - 1] = blend_legacy(
                original[source + back + 1], original[source + side + 1]);
        } else {
            /* MUTE: omit the side pair from the compatibility core. */
            core[target + core_channels - 2] = original[source + back];
            core[target + core_channels - 1] = original[source + back + 1];
        }
    }
    return core_channels;
}

int hdma_downmix_core(const int32_t *original, int channels, int32_t *core)
{
    return hdma_downmix_core_policy(original, channels, core, HDMA_SIDE_BOTH);
}

int hdma_downmix_hierarchical_policy_n(const int32_t *original, int channels,
                                       int32_t *primary, int32_t *core,
                                       HdmaSidePolicy policy, int samples, int depth)
{
    if (!original || !primary || !core || (channels != 7 && channels != 8) ||
        policy < HDMA_SIDE_BOTH || policy > HDMA_SIDE_MUTE || samples < 1 ||
        (depth != 16 && depth != 24)) return -1;
    const int lfe = channels == 8;
    const int primary_channels = channels - 2;
    const int back = lfe ? 4 : 3;
    const int side = lfe ? 6 : 5;
    const int shift = depth == 16 ? 16 : 8;

    for (int n = 0; n < samples; ++n) {
        const size_t s = (size_t)n * (size_t)channels;
        const size_t d = (size_t)n * (size_t)primary_channels;
        int32_t fl=original[s] >> shift;
        int32_t fr=original[s+1] >> shift;
        int32_t fc=original[s+2] >> shift;
        int32_t lfev=lfe ? original[s+3] >> shift : 0;
        int32_t rear_l=original[s+back] >> shift;
        int32_t rear_r=original[s+back+1] >> shift;
        int32_t side_l=original[s+side] >> shift;
        int32_t side_r=original[s+side+1] >> shift;
        int64_t sum_l=(int64_t)side_l+mul_q15_ffmpeg(rear_l,32767);
        int64_t sum_r=(int64_t)side_r+mul_q15_ffmpeg(rear_r,32767);

        /* The hierarchical primary retains the reversible unscaled sum. */
        primary[d]=fl;
        primary[d+1]=fr;
        primary[d+2]=fc;
        if(lfe) primary[d+3]=lfev;
        primary[d+primary_channels-2]=(int32_t)sum_l;
        primary[d+primary_channels-1]=(int32_t)sum_r;

        int32_t out_fl=fl,out_fr=fr,out_fc=fc,out_lfe=lfev,out_sl=rear_l,out_sr=rear_r;
        if(policy==HDMA_SIDE_BOTH) {
            out_fl=mul_q15_ffmpeg(fl,23170);
            out_fr=mul_q15_ffmpeg(fr,23170);
            out_fc=mul_q15_ffmpeg(fc,23170);
            if(lfe) out_lfe=mul_q15_ffmpeg(lfev,23170);
            int64_t pl=sum_l*23170, pr=sum_r*23170;
            int64_t vl=(pl+16384)>>15;
            int64_t vr=(pr+16384)>>15;
            int64_t lo=depth==16 ? -32768 : -8388608;
            int64_t hi=depth==16 ? 32767 : 8388607;
            if(vl<lo) vl=lo;
            if(vl>hi) vl=hi;
            if(vr<lo) vr=lo;
            if(vr>hi) vr=hi;
            out_sl=(int32_t)vl; out_sr=(int32_t)vr;
        } else if(policy==HDMA_SIDE_FRONT) {
            out_fl=blend_legacy(fl,side_l);
            out_fr=blend_legacy(fr,side_r);
        } else if(policy==HDMA_SIDE_REAR) {
            out_sl=blend_legacy(rear_l,side_l);
            out_sr=blend_legacy(rear_r,side_r);
        }

        core[d]=(int32_t)((int64_t)out_fl*((int64_t)1<<shift));
        core[d+1]=(int32_t)((int64_t)out_fr*((int64_t)1<<shift));
        core[d+2]=(int32_t)((int64_t)out_fc*((int64_t)1<<shift));
        if(lfe) core[d+3]=(int32_t)((int64_t)out_lfe*((int64_t)1<<shift));
        core[d+primary_channels-2]=(int32_t)((int64_t)out_sl*((int64_t)1<<shift));
        core[d+primary_channels-1]=(int32_t)((int64_t)out_sr*((int64_t)1<<shift));
    }
    return primary_channels;
}

int hdma_downmix_hierarchical_policy(const int32_t *original, int channels,
                                     int32_t *primary, int32_t *core,
                                     HdmaSidePolicy policy, int depth)
{
    return hdma_downmix_hierarchical_policy_n(original,channels,primary,core,policy,512,depth);
}
