#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
enc=$1; ffmpeg=$2; inspect=$3; fixture=$4
tmp=${TMPDIR:-/tmp}/libhdma-official-structure-$$
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp"
"$fixture" "$tmp/source.wav" 6 48000 24 4096 0x3f musical
"$ffmpeg" -v error -xerror -i "$tmp/source.wav" -f s24le -c:a pcm_s24le -y "$tmp/source.pcm"
"$enc" "$tmp/source.pcm" --raw-pcm --sample-rate 48000 --channels 6 \
  --bit-depth 24 --channel-layout 5.1 --lossless --threads 1 --no-progress \
  -o "$tmp/output.dts" >"$tmp/encode.log"
grep -Eq 'XLL prediction: [0-9]+ core-referenced frames, [0-9]+ full-PCM frames' "$tmp/encode.log"
"$inspect" "$tmp/output.dts"
"$ffmpeg" -v error -i "$tmp/output.dts" -f s24le -c:a pcm_s24le -y "$tmp/decoded.pcm"
cmp "$tmp/source.pcm" "$tmp/decoded.pcm"
