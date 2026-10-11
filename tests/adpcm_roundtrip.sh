#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later
# Independent DCA core/XLL decode verifies optional ADPCM and unmodified PCM.
set -eu
encoder=$1; ffmpeg=$2; ffprobe=$3; fixture=$4; sinefixture=$5
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for config in '2 16 512' '2 24 1024' '6 24 512' '6 24 1024' '8 24 512'; do
    set -- $config
    channels=$1 depth=$2 frame=$3
    extra=''
    if [ "$channels" -eq 8 ]; then extra='--channel-layout 7.1'; fi
    bitrate=1536000
    if [ "$channels" -eq 2 ]; then bitrate=1411200; fi
    "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" 1031
    "$encoder" "$scratch/source.wav" -o "$scratch/output.mka" --adpcm --no-progress \
         --xll-frame-samples "$frame" --bitrate "$bitrate" --threads 1 \
         $extra --overwrite >"$scratch/encode.log"
    grep -q 'Core ADPCM: on' "$scratch/encode.log"
    "$ffmpeg" -v error -xerror -i "$scratch/source.wav" -map 0:a:0 \
        -c:a pcm_s32le -f s32le -y "$scratch/source.pcm"
    "$ffmpeg" -v error -xerror -i "$scratch/output.mka" -map 0:a:0 \
        -c:a pcm_s32le -f s32le -y "$scratch/decoded.pcm"
    cmp "$scratch/source.pcm" "$scratch/decoded.pcm"
    observed=$(
        "$ffprobe" -v error -select_streams a:0 -show_entries stream=channels \
           -of default=nk=1:nw=1 "$scratch/output.mka"
    )
    [ "$observed" = "$channels" ] || {
        echo "ADPCM lossless output channels: expected $channels got $observed" >&2; exit 1;
    }
    "$encoder" "$scratch/source.wav" -o "$scratch/core.dts" --lossy --adpcm \
         --no-progress --xll-frame-samples "$frame" --bitrate "$bitrate" \
         $extra --overwrite >"$scratch/core.log"
    "$ffmpeg" -v error -xerror -f dts -i "$scratch/core.dts" \
        -map 0:a:0 -f null - >/dev/null 2>&1
    printf 'PASS ADPCM: %d-channel PCM%d, %d samples/core frame, core decode and lossless equality\n' \
           "$channels" "$depth" "$frame"
done
# A low-level, correlated signal can use the reconstructed core as an exact
# reference. The general fixture above deliberately includes samples whose
# residuals can overflow the declared 24-bit depth; forced reference should
# reject those rather than silently truncate. Exercise forced mode on a safe
# fixture independently of AUTO's robust fallback.
for channels in 2 6; do
    "$sinefixture" --wav "$scratch/sine.wav" "$channels"
    "$ffmpeg" -v error -xerror -i "$scratch/sine.wav" -map 0:a:0 \
       -c:a pcm_s32le -f s32le -y "$scratch/source.pcm"
    for frame in 512 1024; do
        "$encoder" "$scratch/sine.wav" -o "$scratch/forced.mka" \
            --adpcm --xll-reference core --no-progress --threads 1 \
            --xll-frame-samples "$frame" --bitrate 1536000 \
            --overwrite >"$scratch/forced.log"
        "$ffmpeg" -v error -xerror -i "$scratch/forced.mka" \
            -map 0:a:0 -c:a pcm_s32le -f s32le -y "$scratch/forced.pcm"
        cmp "$scratch/source.pcm" "$scratch/forced.pcm"
        printf 'PASS forced ADPCM core references: %d channels, %d samples/frame\n' "$channels" "$frame"
    done
done
# The ADPCM switch does not introduce nondeterminism across XLL thread counts.
"$fixture" "$scratch/source.wav" 2 48000 24 1031
"$encoder" "$scratch/source.wav" -o "$scratch/thread1.mka" --adpcm \
    --threads 1 --no-progress --overwrite >/dev/null
"$encoder" "$scratch/source.wav" -o "$scratch/thread4.mka" --adpcm \
    --threads 4 --no-progress --overwrite >/dev/null
cmp "$scratch/thread1.mka" "$scratch/thread4.mka"
printf '%s\n' 'PASS ADPCM multithreaded bitstream determinism'
