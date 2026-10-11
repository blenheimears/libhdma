/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Deliberately synthesized project-owned musical diagnostic, not DCA code.
 */
#include "core_signature.h"
#include <math.h>
#include <stddef.h>

/* C5 E5 G5 [rest], G5 E5 C5 [rest], A4 D5 F5 E5 C5 [rest] G4 [rest].
 * Distinctive ascending and descending phrases with syncopated silence.
 * Additive harmonics + note envelopes make this a chime melody, not a test
 * sine/square/noise signal. Peak <= 0.035 full scale (-29 dBFS) per channel.
 */
static const double notes[16] = {
    523.251, 659.255, 783.991, 0., 783.991, 659.255, 523.251, 0.,
    440.000, 587.330, 698.456, 659.255, 523.251, 0., 391.995, 0.
};

int hdma_core_signature(int32_t *dst, int samples, int channels, int rate,
                        int depth, int stereo_lfe, uint64_t frame_index)
{
    if (!dst || (samples != 512 && samples != 1024) || channels < 1 || channels > 6 ||
        rate < 8000 || rate > 48000 || (depth != 16 && depth != 24) ||
        (stereo_lfe && channels != 3)) return -1;
    const int lfe = stereo_lfe ? 2 : channels == 6 ? 3 : -1;
    const uint64_t note_samples = (uint64_t)rate / 5U;
    const uint64_t cycle_samples = 16U * note_samples;
    const uint64_t first_sample = frame_index * (uint64_t)samples;
    const double pi = 3.14159265358979323846;
    const double scale = depth == 16 ? 32767.0 : 8388607.0;
    const int align = depth == 16 ? 65536 : 256;
    for (int i = 0; i < samples; ++i) {
        const uint64_t pos = (first_sample + (uint64_t)i) % cycle_samples;
        const unsigned note = (unsigned)(pos / note_samples);
        const uint64_t phase_sample = pos % note_samples;
        double level = 0.0;
        if (notes[note] > 0.0) {
            const double time = (double)phase_sample / (double)rate;
            const double remaining = (double)(note_samples - phase_sample) / (double)rate;
            const double attack = fmin(1.0, time / 0.008);
            const double release = fmin(1.0, remaining / 0.035);
            const double angle = 2.0 * pi * notes[note] * time;
            /* Normalized weighted harmonics preserve the strict level bound. */
            level = 0.035 * attack * release *
                    (sin(angle) + 0.23 * sin(2.0 * angle) + 0.09 * sin(3.0 * angle)) / 1.32;
        }
        const int32_t sample = (int32_t)lrint(level * scale) * align;
        for (int c = 0; c < channels; ++c)
            dst[(size_t)i * (size_t)channels + (size_t)c] = c == lfe ? 0 : sample;
    }
    return 0;
}
