/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Inspect XLL segment lengths without using the encoder's parser.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned read_bits(const uint8_t *p, size_t bit, unsigned n)
{
    unsigned v = 0;
    for (unsigned j = 0; j < n; ++j)
        v = (v << 1) | ((p[(bit + j) / 8] >> (7 - ((bit + j) % 8))) & 1U);
    return v;
}
int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: inspector STREAM 48000|96000 512|1024 FRAME_COUNT\n");
        return 2;
    }
    unsigned rate = (unsigned)atoi(argv[2]);
    unsigned samples = (unsigned)atoi(argv[3]);
    unsigned expected = (unsigned)atoi(argv[4]);
    if (!rate || (samples != 512 && samples != 1024) || !expected) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END)) return 2;
    long length = ftell(f);
    if (length < 128 || length > 32 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) return 2;
    uint8_t *p = malloc((size_t)length);
    if (!p) return 2;
    if (fread(p, 1, (size_t)length, f) != (size_t)length) return 2;
    fclose(f);
    unsigned found = 0, longest = 0;
    for (size_t i = 0; i + 16 < (size_t)length; ++i) {
        if (memcmp(p + i, "\x41\xa2\x95\x47", 4)) continue;
        unsigned frame_bits = read_bits(p + i, 44, 5) + 1U;
        size_t grid_bit = 53U + frame_bits;
        unsigned grid = read_bits(p + i, grid_bit, 4);
        unsigned segment_log = read_bits(p + i, grid_bit + 4, 4);
        unsigned nseg = grid < 16 ? 1U << grid : 0;
        unsigned segment = segment_log < 16 ? 1U << segment_log : 0;
        unsigned ceiling = rate <= 48000 ? 256U : 512U;
        if (!nseg || segment > ceiling || nseg * segment != samples) {
            fprintf(stderr, "XLL frame %u: %u segments * %u samples; max %u at %u Hz\n",
                    found, nseg, segment, ceiling, rate);
            free(p); return 1;
        }
        if (segment > longest) longest = segment;
        found++;
        i += 3;
    }
    free(p);
    if (found != expected) {
        fprintf(stderr, "XLL frame count %u, expected %u\n", found, expected);
        return 1;
    }
    printf("PASS: %u XLL frames, %u Hz, %u samples/frame, longest segment %u <= %u\n",
           found, rate, samples, longest, rate <= 48000 ? 256U : 512U);
    return 0;
}
