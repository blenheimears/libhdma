#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Exercise order-2/3 prediction, pairwise decorrelation, raw fallback and
# independent bit-exact DCA + XLL decoding on deterministic audio signals.
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
structure=$5
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
passed=0
for mode in quadratic cubic correlated smooth extremes; do
    for channels in 2 6; do
        case "$mode/$channels" in
            quadratic/2) ceiling=85000;;
            quadratic/6) ceiling=155000;;
            cubic/2) ceiling=85000;;
            cubic/6) ceiling=155000;;
            correlated/2) ceiling=95000;;
            correlated/6) ceiling=188000;;
            smooth/2) ceiling=85000;;
            smooth/6) ceiling=160000;;
            *) ceiling=0;;
        esac
        "$fixture" "$scratch/source.wav" "$channels" 48000 24 12288 "$mode"
        "$encoder" encode "$scratch/source.wav" --bitrate 1411200 \
            -o "$scratch/output.mka" --overwrite >/dev/null
        profile=$("$ffprobe" -v error -select_streams a:0 \
            -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.mka")
        [ "$profile" = 'DTS-HD MA' ] || { echo "Not DCA + XLL: $mode/$channels" >&2; exit 1; }
        # Smooth input must exercise an adaptive grid with at least four segments.
        if [ "$mode/$channels" = smooth/6 ]; then
            "$structure" "$scratch/output.mka" 4
        fi
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
            -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
        "$ffmpeg" -hide_banner -nostdin -v error -xerror \
            -err_detect explode+crccheck -i "$scratch/output.mka" \
            -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
        cmp "$scratch/reference.pcm" "$scratch/decoded.pcm" || {
            echo "Lossless PCM mismatch: $mode/$channels" >&2; exit 1;
        }
        size=$(wc -c < "$scratch/output.mka" | tr -d ' ')
        if [ "$ceiling" -gt 0 ] && [ "$size" -ge "$ceiling" ]; then
            echo "Prediction/decorrelation size regression: $mode/$channels: $size >= $ceiling" >&2
            exit 1
        fi
        passed=$((passed+1))
        echo "PASS $mode/$channels: $size bytes, bit-exact"
    done
done
[ "$passed" -eq 10 ]
echo "PASS $passed predictor/decorrelation/boundary tests"
