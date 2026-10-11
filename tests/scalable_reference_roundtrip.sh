#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
set -eu
encoder=$1 ffmpeg=$2 fixture=$3 inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for channels in 2 6 8; do
    case "$channels" in 2) mask=0x3;;6)mask=0x3f;;8)mask=0x63f;;esac
    "$fixture" "$scratch/input.wav" "$channels" 48000 24 1537 "$mask" lsbnoise
    topo=""
    [ "$channels" -eq 8 ] && topo="--xll-7x-hierarchy off"
    # This regression targets scalable single-set syntax; 0.0.45's normal
    # 7.1 receiver-compatible hierarchy intentionally uses full/raw primary data.
    "$encoder" "$scratch/input.wav" --ultra --xll-reference full $topo --threads 2 --no-progress \
        -o "$scratch/output.mka" --overwrite > "$scratch/encode.log"
    "$inspector" "$scratch/output.mka"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
        -c:a pcm_s32le -f s32le -y "$scratch/input.pcm"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/output.mka" -c:a pcm_s32le -f s32le -y "$scratch/output.pcm"
    cmp "$scratch/input.pcm" "$scratch/output.pcm"
    echo "PASS $channels-channel 24-bit scalable XLL exact PCM"
done
# Signed 16-bit content and mixed references continue to use the proven
# non-scalable coding path, with the same independent bit-exact requirement.
"$fixture" "$scratch/input.wav" 6 48000 16 1537 0x3f smooth
"$encoder" "$scratch/input.wav" --xll-reference auto --threads 2 --no-progress \
    -o "$scratch/output.mka" --overwrite > "$scratch/encode.log"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
    -c:a pcm_s16le -f s16le -y "$scratch/input.pcm"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/output.mka" -c:a pcm_s16le -f s16le -y "$scratch/output.pcm"
cmp "$scratch/input.pcm" "$scratch/output.pcm"
echo 'PASS 16-bit non-scalable reference regression'
