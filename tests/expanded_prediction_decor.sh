#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Inspect actual order>=9 and non-default speaker pairs, then independently
# decode and byte-compare all source PCM including short final-frame padding.
set -eu
encoder=$1 ffmpeg=$2 fixture=$3 inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for case in periodic10 crosspair; do
    case "$case" in
        periodic10) channels=2; feature=deeporder;;
        crosspair) channels=6; feature=crosspair;;
    esac
    for depth in 16 24; do
        "$fixture" "$scratch/input.wav" "$channels" 48000 "$depth" 1173 "$case"
        "$encoder" "$scratch/input.wav" --prediction-order 15 --xll-reference full --threads 1 \
            --simd none --no-progress -o "$scratch/output.mka" --overwrite >/dev/null
        "$inspector" "$scratch/output.mka" "$feature"
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
            -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
            -i "$scratch/output.mka" -f s32le -c:a pcm_s32le \
            -y "$scratch/decoded.pcm"
        cmp "$scratch/reference.pcm" "$scratch/decoded.pcm"
        "$encoder" "$scratch/input.wav" --prediction-order 15 --xll-reference full --threads 4 \
            --simd auto --no-progress -o "$scratch/parallel.mka" --overwrite >/dev/null
        cmp "$scratch/output.mka" "$scratch/parallel.mka"
        echo "PASS $case ${depth}-bit: real $feature syntax, exact PCM and scalar/parallel SIMD identity"
    done
done
# 7.1 retains all eight lossless source speakers even with freely selected
# decorrelation pairings; only the separate core may be downmixed.
"$fixture" "$scratch/input.wav" 8 48000 24 1173 0x63f periodic10
"$encoder" "$scratch/input.wav" --threads 1 --no-progress \
    -o "$scratch/output.mka" --overwrite >/dev/null
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
    -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/output.mka" -f s32le -c:a pcm_s32le \
    -y "$scratch/decoded.pcm"
cmp "$scratch/reference.pcm" "$scratch/decoded.pcm"
echo 'PASS 7.1 original channels bit-identical, partial final frame'
