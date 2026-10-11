#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Assert the bitstream actually contains parameter reuse AND common-channel
# allocations, rather than only checking that the implementation has branches.
# Decode with an independent FFmpeg binary and compare every PCM byte.
set -eu
encoder=$1
ffmpeg=$2
fixture=$3
inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for signal in silence smooth correlated; do
    "$fixture" "$scratch/input.wav" 6 48000 24 12288 "$signal"
    "$encoder" "$scratch/input.wav" --xll-reference full --threads 1 \
        --no-progress --simd none -o "$scratch/output.mka" --overwrite > /dev/null
    case "$signal" in
        silence) "$inspector" "$scratch/output.mka" common ;;
        correlated) "$inspector" "$scratch/output.mka" reuse ;;
    esac
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
        -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror \
        -err_detect explode+crccheck -i "$scratch/output.mka" \
        -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
    cmp "$scratch/reference.pcm" "$scratch/decoded.pcm"
    size=$(wc -c < "$scratch/output.mka" | tr -d ' ')
    case "$signal" in
        # The reference-compatible predictor policy intentionally forbids fixed
        # orders 1..3. This synthetic polynomial was specifically constructed
        # to make those predictors unrealistically efficient, so its historical
        # 73 kB ceiling is no longer meaningful. Keep a broad guard against an
        # unrelated entropy regression while adaptive prediction handles it.
        smooth) [ "$size" -lt 175000 ] || { echo "Reference-compatible smooth size regression: $size" >&2; exit 1; } ;;
        correlated) [ "$size" -lt 165000 ] || { echo "Reference-compatible reuse size regression: $size" >&2; exit 1; } ;;
    esac
    printf 'PASS %-10s: %s bytes, bit-identical decoded PCM\n' "$signal" "$size"
done
