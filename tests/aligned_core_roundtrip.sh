#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Verify the hidden one-frame core-analysis lookahead, exact PCM, raw-stream
# framing, deterministic explicit/default selection, and measurable savings.
set -eu
encoder=$1 ffmpeg=$2 ffprobe=$3 fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$fixture" "$scratch/source.wav" 6 48000 24 8192 0x3f musical
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
    -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"
for mode in default explicit legacy full; do
    case "$mode" in
        default) args=;;
        explicit) args=--core-lookahead;;
        legacy) args=--no-core-lookahead;;
        full) args=--xll-reference\ full;;
    esac
    # shellcheck disable=SC2086
    "$encoder" "$scratch/source.wav" -o "$scratch/$mode.mka" \
        --bitrate 1536000 --threads 1 --simd none --no-progress --overwrite $args \
        > "$scratch/$mode.log"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/$mode.mka" -f s32le -c:a pcm_s32le -y "$scratch/$mode.pcm"
    cmp "$scratch/source.pcm" "$scratch/$mode.pcm"
done
cmp "$scratch/default.mka" "$scratch/explicit.mka"
grep -q 'core alignment: one-frame lookahead' "$scratch/default.log"
grep -q 'core alignment: legacy' "$scratch/legacy.log"
grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames, [1-9][0-9]* full-PCM frames' "$scratch/default.log"
[ "$(wc -c < "$scratch/default.mka")" -lt "$(wc -c < "$scratch/legacy.mka")" ]
[ "$(wc -c < "$scratch/default.mka")" -lt "$(wc -c < "$scratch/full.mka")" ]
profile=$($ffprobe -v error -select_streams a:0 -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/default.mka")
[ "$profile" = 'DTS-HD MA' ]
# Raw output has exactly one packet per source frame and no codec pre-roll.
"$encoder" "$scratch/source.wav" -o "$scratch/aligned.dts" --bitrate 1536000 \
    --threads 1 --simd none --no-progress --overwrite >/dev/null
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/aligned.dts" -f s32le -c:a pcm_s32le -y "$scratch/raw.pcm"
cmp "$scratch/source.pcm" "$scratch/raw.pcm"
# A one-sample stream remains self-contained and exact.
"$fixture" "$scratch/tiny.wav" 2 48000 24 1 0x3 smooth
"$encoder" "$scratch/tiny.wav" -o "$scratch/tiny.mka" --threads 1 --simd none \
    --no-progress --overwrite > "$scratch/tiny.log"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/tiny.mka" \
    -f s32le -c:a pcm_s32le -y "$scratch/tiny.pcm"
[ "$(wc -c < "$scratch/tiny.pcm")" -eq 8 ]
grep -q '0 core-referenced frames, 1 full-PCM frames' "$scratch/tiny.log"
echo 'PASS one-frame core lookahead alignment, exact MKA/raw PCM, fallback and size regression'
