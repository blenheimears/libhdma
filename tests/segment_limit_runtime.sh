#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
set -eu
encoder=$1; inspector=$2
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
# The current DCA core frontend does not support 96-kHz 5.1 at 1536 kb/s.
# Inspect both legal 48-kHz frame sizes; >48-kHz logic remains rate-dependent.
for rate in 48000; do
    for samples in 512 1024; do
        # Silence exercises the entropy/raw fallback, and a deterministic
        # PCM ramp exercises prediction and a non-silent multichannel stream.
        for signal in silence ramp; do
            bytes=$((samples * 6 * 3 * 4))
            if [ "$signal" = silence ]; then
                dd if=/dev/zero of="$scratch/input.pcm" bs="$bytes" count=1 2>/dev/null
            else
                # A reproducible, full-width, 24-bit per-channel pattern.
                awk -v n=$((samples * 6 * 4)) 'BEGIN {
                    for (i=0; i<n; ++i) {
                        v=(i * 7919 + (i % 17) * 791) % 16777216;
                        printf("%c%c%c", v % 256, int(v / 256) % 256, int(v / 65536) % 256);
                    }
                }' > "$scratch/input.pcm"
            fi
            "$encoder" "$scratch/input.pcm" --raw-pcm --sample-rate "$rate" \
                --channels 6 --bit-depth 24 --channel-layout 5.1 --lossless \
                --xll-frame-samples "$samples" --no-progress --threads 1 \
                -o "$scratch/output.dts" --overwrite > "$scratch/log"
            "$inspector" "$scratch/output.dts" "$rate" "$samples" 4
        done
    done
done
