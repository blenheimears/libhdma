#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
set -eu
encoder=$1 ffmpeg=$2 fixture=$3 inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$fixture" "$scratch/source.wav" 2 48000 24 1537 0x3 lsbnoise
"$ffmpeg" -nostdin -v error -xerror -i "$scratch/source.wav" -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"
"$encoder" "$scratch/source.wav" --threads 1 --simd none --no-progress \
    --xll-reference full -o "$scratch/fast.mka" >"$scratch/fast.log"
"$inspector" "$scratch/fast.mka" none
"$encoder" "$scratch/source.wav" --threads 1 --simd none --no-progress \
    --ultra --xll-reference full -o "$scratch/ultra.mka" >"$scratch/ultra.log"
"$inspector" "$scratch/ultra.mka"
for kind in fast ultra; do
    "$ffmpeg" -nostdin -v error -xerror -err_detect explode+crccheck \
      -i "$scratch/$kind.mka" -f s32le -c:a pcm_s32le -y "$scratch/$kind.pcm"
    cmp "$scratch/source.pcm" "$scratch/$kind.pcm"
done
grep -q 'ultra: off' "$scratch/fast.log"
grep -q 'ultra: on' "$scratch/ultra.log"
echo 'PASS: default disabled scalable split; --ultra re-enables; both bit-exact'
