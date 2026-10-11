/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Inspect native XLL bitstream signalling, independently of the encoder.
 * Actual losslessness is separately verified by FFmpeg decode + PCM cmp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *bytes;
    size_t size, bit;
    int error;
} Reader;

static unsigned get(Reader *r, unsigned width)
{
    unsigned v = 0;
    if (width > 32 || r->bit > r->size * 8 ||
        width > r->size * 8 - r->bit) {
        r->error = 1;
        return 0;
    }
    while (width--) {
        size_t p = r->bit++;
        v = (v << 1) | ((r->bytes[p / 8] >> (7 - (p & 7))) & 1U);
    }
    return v;
}

static int inspect(const uint8_t *file, size_t length,
                   size_t offset, unsigned *high, unsigned *hybrid,
                   unsigned *reused, unsigned *shared,
                   unsigned *deep, unsigned *cross, unsigned *maxorder)
{
    Reader r = {file, length, offset * 8, 0};
    if (get(&r, 32) != 0x41a29547U || get(&r, 4) != 0)
        return -1;
    unsigned common = get(&r, 8) + 1U;
    unsigned frame_size_bits = get(&r, 5) + 1U;
    unsigned frame_size = get(&r, frame_size_bits) + 1U;
    if (offset + frame_size > length || get(&r, 4) != 0) return -1;
    unsigned log_segments = get(&r, 4);
    unsigned log_samples = get(&r, 4);
    unsigned navi_bits = get(&r, 5) + 1U;
    if (log_segments>4 ||
        ((log_samples + log_segments != 9) && (log_samples + log_segments != 10)) ||
        navi_bits < 1 || navi_bits > 20 || common > frame_size) return -1;
    unsigned segments = 1U << log_segments;
    r.bit = (offset + common) * 8;
    unsigned subheader = get(&r, 10) + 1U;
    unsigned channels = get(&r, 4) + 1U;
    if (channels > 8 || offset + common + subheader > length) return -1;
    get(&r, channels); /* core-reference flags */
    unsigned depth = get(&r, 5) + 1U;
    get(&r, 5); /* storage depth */
    get(&r, 4); /* sample rate */
    get(&r, 2 + 2 + 1 + 1 + 1); /* modifiers, replacement, hierarchy */
    unsigned has_mask = get(&r, 1);
    if (!has_mask) return -1;
    get(&r, channels > 6 ? 9 : 6); /* speaker mask */
    unsigned decor = get(&r, 1);
    if (decor) {
        unsigned channel_bits = channels <= 2 ? 1U : channels <= 4 ? 2U : 3U;
        unsigned order[8];
        for (unsigned ch = 0; ch < channels; ch++)
            order[ch]=get(&r,channel_bits);
        for (unsigned pair = 0; pair < channels / 2; pair++) {
            unsigned enabled=get(&r, 1);
            if(enabled) {
                get(&r, 7);
                if(channels==6) {
                    unsigned a=order[2*pair],b=order[2*pair+1];
                    /* Previous six-channel implementation only tried
                     * 1->2, 4->5 and 0->3. */
                    if(!((a==1&&b==2)||(a==4&&b==5)||(a==0&&b==3)))
                        ++*cross;
                }
            }
        }
    }
    for (unsigned ch = 0; ch < channels; ch++) {
        unsigned order=get(&r,4);
        if(order>*maxorder) *maxorder=order;
        if(order>=3) ++*high;
        if(order>=9) ++*deep;
    }
    size_t nav = offset + common + subheader;
    r.bit = nav * 8;
    unsigned segsizes[16];
    for (unsigned seg = 0; seg < segments; seg++)
        segsizes[seg] = get(&r, navi_bits) + 1U;
    size_t segpos = nav + (segments * navi_bits + 7) / 8 + 2;
    for (unsigned seg = 0; seg < segments; seg++) {
        if (segpos + segsizes[seg] > offset + frame_size) return -1;
        r.bit = segpos * 8;
        unsigned reuse = seg && get(&r, 1);
        if(reuse) ++*reused;
        if (!reuse) {
            unsigned common_alloc = get(&r, 1);
            if(common_alloc) ++*shared;
            for (unsigned ch = 0; ch < (common_alloc ? 1U : channels); ch++) {
                unsigned rice = get(&r, 1);
                if (rice && !common_alloc && get(&r, 1)) {
                    get(&r, depth == 16 ? 4 : 5);
                    (*hybrid)++;
                }
            }
        }
        segpos += segsizes[seg];
    }
    return r.error ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "highorder") &&
                      strcmp(argv[2], "hybrid") &&
                      strcmp(argv[2], "reuse") &&
                      strcmp(argv[2], "common") &&
                      strcmp(argv[2], "deeporder") &&
                      strcmp(argv[2], "crosspair") &&
                      strcmp(argv[2], "limit0") && strcmp(argv[2], "limit10") &&
                      strcmp(argv[2], "limit15"))) {
        fprintf(stderr, "usage: test_xll_tools FILE highorder|hybrid|reuse|common|deeporder|crosspair|limit0|limit10|limit15\n");
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    if (fseek(fp, 0, SEEK_END)) { fclose(fp); return 1; }
    long length = ftell(fp);
    if (length < 0 || length > 128 * 1024 * 1024 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp); return 1;
    }
    uint8_t *data = malloc((size_t)length + 1);
    if (!data) { fclose(fp); return 1; }
    int result = 1;
    if (fread(data, 1, (size_t)length, fp) != (size_t)length) goto done;
    unsigned frames = 0, high = 0, hybrid = 0, reused = 0, shared = 0;
    unsigned deep = 0, cross = 0, maxorder = 0;
    for (size_t i = 0; i + 16 < (size_t)length; i++) {
        if (memcmp(data + i, "\x41\xa2\x95\x47", 4)) continue;
        if (inspect(data, (size_t)length, i, &high, &hybrid, &reused, &shared,
                    &deep, &cross, &maxorder)) {
            fprintf(stderr, "Invalid XLL structure at offset %zu\n", i);
            goto done;
        }
        ++frames;
        i += 3;
    }
    if (!frames || (!strcmp(argv[2], "highorder") && !high) ||
                   (!strcmp(argv[2], "hybrid") && !hybrid) ||
                   (!strcmp(argv[2], "reuse") && !reused) ||
                   (!strcmp(argv[2], "common") && !shared) ||
                   (!strcmp(argv[2], "deeporder") && !deep) ||
                   (!strcmp(argv[2], "crosspair") && !cross) ||
                   (!strcmp(argv[2], "limit0") && maxorder!=0) ||
                   (!strcmp(argv[2], "limit10") && maxorder>10) ||
                   (!strcmp(argv[2], "limit15") && maxorder>15)) {
        fprintf(stderr, "Feature absent: frames=%u higher-order=%u hybrid=%u reused=%u common=%u order9-15=%u nondefault-pairs=%u max-adaptive-order=%u\n",
                frames, high, hybrid, reused, shared, deep, cross, maxorder);
        goto done;
    }
    printf("PASS XLL tools: frames=%u highorder=%u hybrid=%u reuse=%u common=%u order9-15=%u nondefault-pairs=%u max-adaptive-order=%u\n",
           frames, high, hybrid, reused, shared, deep, cross, maxorder);
    result = 0;
done:
    free(data);
    fclose(fp);
    return result;
}
