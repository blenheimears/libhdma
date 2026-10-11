/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later. */
#ifndef LIBHDMA_MATROSKA_H
#define LIBHDMA_MATROSKA_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { uint64_t ms, position; } MkaCue;
typedef struct {
    FILE *file;
    int sample_rate, channels, started;
    uint64_t cluster_ms;
    int64_t segment_size_pos, segment_payload_pos, cluster_size_pos;
    int64_t cluster_payload_pos, seekhead_pos, duration_pos;
    uint64_t info_pos, tracks_pos;
    MkaCue *cues;
    size_t cue_count, cue_capacity;
} MkaWriter;
int mka_start(MkaWriter *m, FILE *file, int sample_rate, int channels);
int mka_packet(MkaWriter *m, const uint8_t *packet, size_t size, uint64_t sample_position);
int mka_packet_trim(MkaWriter *m, const uint8_t *packet, size_t size,
                    uint64_t sample_position, int discard_frames);
/* Exact PCM samples per channel (or padded DCA core samples for core-only).
 * Finalizes sizes, metadata and seek index; requires seekable output file. */
int mka_finish(MkaWriter *m, uint64_t samples);
void mka_dispose(MkaWriter *m);
#endif
