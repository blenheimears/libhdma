/* Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later
 * Scalar analysis MDCT. Used exclusively for perceptual noise estimation,
 * not the core reconstruction filterbank or bitstream syntax.
 */
#include "core_internal.h"
void standalone_mdct(int32_t dst[256], const int32_t src[512]) {
    /* Recurrence per frequency avoids 131072 expensive transcendental calls. */
    for (int k=0; k<256; k++) {
        double phase=M_PI/256.0*(256.5)*(k+0.5);
        double step=M_PI/256.0*(k+0.5);
        double cs=cos(phase), sn=sin(phase), dc=cos(step), ds=sin(step);
        double sum=0.0;
        for (int n=0; n<512; n++) {
            sum+=(double)src[n]*cs;
            double nextcs=cs*dc-sn*ds;
            sn=sn*dc+cs*ds;
            cs=nextcs;
        }
        /* FFT based integer reference transform differs in intermediate
           scaling. Saturate rather than risk implementation-defined casts. */
        sum/=8.0;
        if(sum>INT32_MAX) sum=INT32_MAX;
        if(sum<INT32_MIN) sum=INT32_MIN;
        dst[k]=(int32_t)llround(sum);
    }
}
