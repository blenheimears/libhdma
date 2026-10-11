#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Verify the 7.1 receiver-compatible default mirrors the supplied working
# stream: stable embedded-5.1 hierarchy, -3 dB matrix and core-referenced
# primary after the self-contained startup frame. The hardware-test profile
# remains a deliberately minimal single-set control stream.
set -eu
encoder=$1 ffmpeg=$2 fixture=$3 inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$fixture" "$scratch/source.wav" 8 48000 24 4096 0x63f musical
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
    -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"

"$encoder" "$scratch/source.wav" -o "$scratch/default.mka" --threads 1 --simd none \
    --no-progress --overwrite >"$scratch/default.log" 2>"$scratch/default.err"
"$encoder" "$scratch/source.wav" -o "$scratch/full.mka" --xll-reference full \
    --threads 1 --simd none --no-progress --overwrite >"$scratch/full.log" 2>"$scratch/full.err"
grep -q '7.1 reference-compatible default: core-residual 5.1 primary XLL + Lsr/Rsr extension' "$scratch/default.err"
grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames, 1 full-PCM frames' "$scratch/default.log"
grep -q 'core alignment: one-frame lookahead' "$scratch/default.log"
grep -q '7.1 XLL topology: embedded 5.1 primary + Lsr/Rsr extension (stable)' "$scratch/default.log"
"$inspector" "$scratch/default.mka" 8 hier-stable
"$inspector" "$scratch/full.mka" 8 single
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/default.mka" -f s32le -c:a pcm_s32le -y "$scratch/default.pcm"
cmp "$scratch/source.pcm" "$scratch/default.pcm"

# Explicit AUTO remains available so compression experiments are not removed.
"$encoder" "$scratch/source.wav" -o "$scratch/auto.mka" --xll-reference auto \
    --threads 1 --simd none --no-progress --overwrite >"$scratch/auto.log" 2>"$scratch/auto.err"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/auto.mka" -f s32le -c:a pcm_s32le -y "$scratch/auto.pcm"
cmp "$scratch/source.pcm" "$scratch/auto.pcm"

# Hardware test deliberately avoids residual/core dependence and every optional
# predictor/decorrelation/reuse path, forcing the raw linear XLL fallback.
"$encoder" "$scratch/source.wav" -o "$scratch/hw.mka" --xll-hardware-test \
    --threads 1 --simd none --no-progress --overwrite >"$scratch/hw.log" 2>"$scratch/hw.err"
grep -q 'XLL hardware test: full-PCM single-set raw linear coding' "$scratch/hw.err"
grep -q 'adaptive-order<=0 fixed-order<=0 parameter-reuse=off decorrelation=off max-segments=2 raw-linear=yes' "$scratch/hw.log"
grep -q 'XLL prediction: 0 core-referenced frames' "$scratch/hw.log"
"$inspector" "$scratch/hw.mka" 8 single
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/hw.mka" -f s32le -c:a pcm_s32le -y "$scratch/hw.pcm"
cmp "$scratch/source.pcm" "$scratch/hw.pcm"
[ "$(wc -c < "$scratch/hw.mka")" -gt "$(wc -c < "$scratch/default.mka")" ]
if "$encoder" "$scratch/source.wav" -o "$scratch/bad.dts" --lossy --xll-hardware-test \
    --no-progress --overwrite >/dev/null 2>&1; then exit 1; fi
echo 'PASS 7.1 reference-compatible embedded-5.1 core-residual default and minimal single-set hardware control'
