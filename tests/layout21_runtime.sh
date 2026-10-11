#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Verify 2.1 is distinct from 3.0 in core and XLL. Explicit DCA demuxing
# prevents FFmpeg probe heuristics from rejecting tiny (3-frame) test assets.
# A separate full-length auto-probe regression validates natural detection.
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$encoder" --help > "$scratch/help"
for name in 'mono' '1.0' '2.0' '2.1' '--lossless'; do
    grep -Fq -- "$name" "$scratch/help" || { echo "missing help option $name" >&2; exit 1; }
done
for bits in 16 24; do
    "$fixture" "$scratch/21.wav" 3 48000 "$bits" 1536 0xb
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/21.wav" \
        -f "s${bits}le" -c:a "pcm_s${bits}le" -y "$scratch/input.pcm"
    for samples in 512 1024; do
        cat "$scratch/input.pcm" | "$encoder" - --raw-pcm --sample-rate 48000 \
            --channels 3 --bit-depth "$bits" --channel-layout 2.1 \
            --xll-frame-samples "$samples" --lossless --no-progress --threads 2 \
            -o "$scratch/21.dts" --overwrite >/dev/null
        layout=$("$ffprobe" -f dts -v error -select_streams a:0 -show_entries stream=channel_layout \
            -of default=nw=1:nk=1 "$scratch/21.dts")
        profile=$("$ffprobe" -f dts -v error -select_streams a:0 -show_entries stream=profile \
            -of default=nw=1:nk=1 "$scratch/21.dts")
        [ "$layout" = 2.1 ] && [ "$profile" = 'DTS-HD MA' ] || {
            echo "wrong decoded 2.1 profile/layout: $profile / $layout" >&2; exit 1; }
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -f dts -i "$scratch/21.dts" \
            -f "s${bits}le" -c:a "pcm_s${bits}le" -y "$scratch/decoded.pcm"
        head -c "$(wc -c < "$scratch/input.pcm")" "$scratch/decoded.pcm" > "$scratch/trimmed.pcm"
        cmp "$scratch/input.pcm" "$scratch/trimmed.pcm" || {
            echo "2.1 PCM changed, bits=$bits samples=$samples" >&2; exit 1; }
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -core_only 1 -f dts -i "$scratch/21.dts" \
            -f s16le -y "$scratch/core.pcm"
        test -s "$scratch/core.pcm"
        printf 'PASS 2.1: %s-bit / %s samples, exact lossless PCM and core decode\n' "$bits" "$samples"
    done
done
# 3.0 must remain distinct from 2.1.
cat "$scratch/input.pcm" | "$encoder" - --raw-pcm --sample-rate 48000 \
    --channels 3 --bit-depth 24 --channel-layout 3.0 --lossless --no-progress \
    -o "$scratch/30.dts" >/dev/null
layout=$("$ffprobe" -f dts -v error -select_streams a:0 -show_entries stream=channel_layout \
    -of default=nw=1:nk=1 "$scratch/30.dts")
[ "$layout" = '3.0' ] || { echo "3.0 was changed to $layout" >&2; exit 1; }
