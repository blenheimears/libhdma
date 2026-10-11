/* Copyright (C) 2026 the contributors to the libhdma project
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Human-readable progress is kept separate from encoding and muxing.
 */
#define _POSIX_C_SOURCE 200809L
#include "progress.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static double seconds_between(struct timespec newer, struct timespec older)
{
    return (double)(newer.tv_sec - older.tv_sec) +
           (double)(newer.tv_nsec - older.tv_nsec) / 1000000000.0;
}

static void render(HdmaProgress *p, uint64_t done, uint64_t output_bytes, int force)
{
    struct timespec now;
    if (!p || !p->active || !p->total) return;
    if (done > p->total) done = p->total;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = seconds_between(now, p->started);
    double fraction = (double)done / (double)p->total;
    unsigned percent = (unsigned)(fraction * 100.0);
    if (percent > 100) percent = 100;
    if (!force) {
        if (p->tty) {
            /* Avoid hundreds of terminal refreshes per second. */
            if (seconds_between(now, p->last_update) < 0.10) return;
        } else if ((int)(percent / 10) <= p->last_percent / 10) {
            return;
        }
    }
    p->done = done;
    p->output_bytes = output_bytes;
    p->last_percent = (int)percent;
    p->last_update = now;

    /* The final partial frame counts only its actual source samples. */
    uint64_t samples = done == p->total ? p->source_samples : done * (uint64_t)p->frame_samples;
    long double original_bytes = (long double)samples * p->channels * (p->depth / 8);
    long double ratio = original_bytes > 0.0L ? (long double)output_bytes / original_bytes : 0.0L;
    double speed = elapsed > 0.001 && p->rate > 0
                 ? (double)samples / ((double)p->rate * elapsed) : 0.0;
    uint64_t eta = done && elapsed > 0.001 && done < p->total
                 ? (uint64_t)((double)(p->total - done) * elapsed / (double)done) : 0;
    char ratio_text[64];
    if (!samples) snprintf(ratio_text, sizeof(ratio_text), "---");
    else snprintf(ratio_text, sizeof(ratio_text), "%.3Lf", ratio);

    if (p->tty) {
        char message[256];
        struct winsize ws;
        unsigned columns = ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col
                         ? ws.ws_col : 80;
        snprintf(message, sizeof(message), "Encoding %5.1f%%  %" PRIu64 "/%" PRIu64
                 " frames  %.2fx  ETA %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64
                 "  ratio=%s", 100.0 * fraction, done, p->total, speed,
                 eta / 3600, (eta / 60) % 60, eta % 60, ratio_text);
        if (strlen(message) >= columns) {
            snprintf(message, sizeof(message), "Encoding %5.1f%%  %.2fx  ratio=%s",
                     100.0 * fraction, speed, ratio_text);
        }
        if (strlen(message) >= columns) {
            snprintf(message, sizeof(message), "%5.1f%% r=%s", 100.0 * fraction, ratio_text);
        }
        /* Avoid automatic line wrapping even on unusually narrow terminals. */
        if (columns <= 1) message[0] = '\0';
        else if (strlen(message) >= columns) message[columns - 1] = '\0';
        fprintf(stderr, "\r\033[2K%s", message);
        fflush(stderr);
    } else {
        /* Redirected logs are line-oriented: no CR, no ANSI control codes. */
        fprintf(stderr, "hdmaenc: encoding %u%% (%" PRIu64 "/%" PRIu64
                " frames)  %.2fx  ratio=%s\n",
                percent, done, p->total, speed, ratio_text);
    }
}

void hdma_progress_begin(HdmaProgress *p, uint64_t source_samples, int rate,
                         int channels, int depth, int enabled,int ratio_container,int frame_samples)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->enabled = enabled && source_samples > 0 && channels > 0 && (depth == 16 || depth == 24);
    if (!p->enabled) return;
    p->ratio_container = ratio_container;
    p->source_samples = source_samples;
    p->frame_samples=frame_samples;
    p->total = source_samples / (uint64_t)frame_samples + (source_samples % (uint64_t)frame_samples != 0);
    p->rate = rate;
    p->channels = channels;
    p->depth = depth;
    p->tty = isatty(STDERR_FILENO) != 0;
    p->last_percent = -10;
    clock_gettime(CLOCK_MONOTONIC, &p->started);
    p->last_update = p->started;
    p->active = 1;
    render(p, 0, 0, 1);
}

void hdma_progress_advance(HdmaProgress *p, uint64_t completed_frames,
                           uint64_t codec_bytes,uint64_t container_bytes)
{
    if (!p || !p->active) return;
    /* Render 100% only after final Matroska metadata has been written. */
    if (completed_frames >= p->total) return;
    render(p, completed_frames, p->ratio_container?container_bytes:codec_bytes, 0);
}

void hdma_progress_finish(HdmaProgress *p, uint64_t final_codec_bytes,
                          uint64_t final_container_bytes)
{
    if (!p || !p->active) return;
    render(p, p->total, p->ratio_container?final_container_bytes:final_codec_bytes, 1);
    if (p->tty) fputc('\n', stderr);
    p->active = 0;
}

void hdma_progress_abort(HdmaProgress *p)
{
    if (!p || !p->active) return;
    if (p->tty) {
        fputs("\r\033[2K", stderr);
        fflush(stderr);
    }
    p->active = 0;
}
