#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Verify the delayed DCA-core lookahead works across 7.0 and 7.1 speaker maps,
# preserves original eight/seven-channel PCM, and never changes the core-only
# downmix policy. FFmpeg is an independent TEST decoder, not an encoder backend.
set -eu
encoder=$1 ffmpeg=$2 ffprobe=$3 fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for channels in 7 8; do
    if [ "$channels" -eq 7 ]; then mask=0x637; core_channels=5
    else mask=0x63f; core_channels=6; fi
    for depth in 16 24; do
        if [ "$depth" -eq 16 ]; then format=s16le; codec=pcm_s16le
        else format=s32le; codec=pcm_s32le; fi
        # The first source frame must remain self-contained, and the last
        # partial frame must be trimmed to the original sample count.
        "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" 513 "$mask" smooth
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
            -f "$format" -c:a "$codec" -y "$scratch/source.pcm"
        "$encoder" "$scratch/source.wav" -o "$scratch/default.mka" \
            --threads 1 --simd none --no-progress --overwrite >"$scratch/default.log"
        "$encoder" "$scratch/source.wav" -o "$scratch/explicit.mka" \
            --core-lookahead --threads 1 --simd none --no-progress --overwrite >"$scratch/explicit.log"
        cmp "$scratch/default.mka" "$scratch/explicit.mka"
        grep -q 'core alignment: one-frame lookahead' "$scratch/default.log"
        if [ "$channels" -eq 8 ] && [ "$depth" -eq 24 ]; then
            grep -q '7.1 XLL topology: embedded 5.1 primary + Lsr/Rsr extension (stable)' "$scratch/default.log"
        fi
        [ "$("$ffprobe" -v error -select_streams a:0 -show_entries stream=channels -of default=nw=1:nk=1 "$scratch/default.mka")" -eq "$channels" ]
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
            -i "$scratch/default.mka" -f "$format" -c:a "$codec" -y "$scratch/decoded.pcm"
        cmp "$scratch/source.pcm" "$scratch/decoded.pcm"
        [ "$(wc -c < "$scratch/decoded.pcm")" -eq "$(wc -c < "$scratch/source.pcm")" ]
        echo "PASS aligned $channels-channel $depth-bit two-frame partial-tail bit-exact"
    done
    # Use a predictable tonal fixture on which time-aligned core residuals
    # are genuinely smaller than the previous unaligned representation.
    "$fixture" "$scratch/music.wav" "$channels" 48000 24 8192 "$mask" musical
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/music.wav" \
        -f s32le -c:a pcm_s32le -y "$scratch/music.pcm"
    "$encoder" "$scratch/music.wav" -o "$scratch/aligned.mka" \
        --xll-reference auto --threads 1 --simd none --no-progress --overwrite >"$scratch/aligned.log"
    "$encoder" "$scratch/music.wav" -o "$scratch/legacy.mka" \
        --xll-reference auto --no-core-lookahead --threads 1 --simd none --no-progress --overwrite >"$scratch/legacy.log"
    "$encoder" "$scratch/music.wav" -o "$scratch/full.mka" \
        --xll-reference full --threads 1 --simd none --no-progress --overwrite >"$scratch/full.log"
    grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames' "$scratch/aligned.log"
    [ "$(wc -c < "$scratch/aligned.mka")" -lt "$(wc -c < "$scratch/legacy.mka")" ]
    [ "$(wc -c < "$scratch/aligned.mka")" -le "$(wc -c < "$scratch/full.mka")" ]
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/aligned.mka" -f s32le -c:a pcm_s32le -y "$scratch/musicdecoded.pcm"
    cmp "$scratch/music.pcm" "$scratch/musicdecoded.pcm"
    # The independently playable core remains a 5.0/5.1 downmix, not 7.x.
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -core_only 1 \
        -i "$scratch/aligned.mka" -f s32le -c:a pcm_s32le -y "$scratch/core.pcm"
    [ "$(wc -c < "$scratch/core.pcm")" -eq "$((8192*core_channels*4))" ]
    echo "PASS aligned $channels-channel 24-bit reference savings and core layout"
done
# All four side-channel policies must affect ONLY the lossy core; even when
# the corresponding residuals are not selected, XLL PCM remains unchanged.
"$fixture" "$scratch/eight.wav" 8 48000 24 2048 0x63f musical
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/eight.wav" \
    -f s32le -c:a pcm_s32le -y "$scratch/eight.pcm"
for policy in both front rear mute; do
    "$encoder" "$scratch/eight.wav" --core-side "$policy" \
        --threads 1 --simd none --no-progress --overwrite \
        -o "$scratch/policy-$policy.mka" >"$scratch/policy-$policy.log" 2>"$scratch/policy-$policy.err"
    # Every side policy remains time-aligned.  The nonstandard front/rear/mute
    # policies use the single-set representation because the reference two-set
    # hierarchy's embedded matrix is defined only for BOTH.
    grep -q 'core alignment: one-frame lookahead' "$scratch/policy-$policy.log"
    if [ "$policy" = both ]; then
        grep -q '7.1 XLL topology: embedded 5.1 primary + Lsr/Rsr extension (stable)' "$scratch/policy-$policy.log"
    else
        grep -q '7.1 XLL topology: single-set' "$scratch/policy-$policy.log"
    fi
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/policy-$policy.mka" -f s32le -c:a pcm_s32le -y "$scratch/policy-$policy.pcm"
    cmp "$scratch/eight.pcm" "$scratch/policy-$policy.pcm"
done
# The implicit and explicitly spelled normal side policy must be bit-identical.
"$encoder" "$scratch/eight.wav" --threads 1 --simd none --no-progress --overwrite \
    -o "$scratch/policy-default.mka" >"$scratch/policy-default.log" 2>"$scratch/policy-default.err"
cmp "$scratch/policy-default.mka" "$scratch/policy-both.mka"
# Spelling other semantic defaults must also be byte-identical.  This catches
# frontends that serialize every default option instead of omitting it.
"$encoder" "$scratch/eight.wav" --xll-reference auto --xll-7x-hierarchy on \
    --core-lookahead --xll-parameter-reuse auto --xll-decorrelation on \
    --xll-max-segments 16 --prediction-order 10 --max-fixed-prediction-order 0 \
    --xll-frame-samples 512 --no-adpcm --bitrate 1536000 \
    --threads 1 --simd none --no-progress --overwrite \
    -o "$scratch/explicit-defaults.mka" >"$scratch/explicit-defaults.log" 2>"$scratch/explicit-defaults.err"
cmp "$scratch/policy-default.mka" "$scratch/explicit-defaults.mka"
grep -q 'core alignment: one-frame lookahead' "$scratch/explicit-defaults.log"
# Core-reference-only tests speaker mapping and the first-frame full fallback.
"$encoder" "$scratch/eight.wav" --xll-reference core --core-side both \
    --threads 1 --simd none --no-progress --overwrite \
    -o "$scratch/forced.mka" >"$scratch/forced.log"
grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames, 1 full-PCM frames' "$scratch/forced.log"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/forced.mka" -f s32le -c:a pcm_s32le -y "$scratch/forced.pcm"
cmp "$scratch/eight.pcm" "$scratch/forced.pcm"
"$encoder" "$scratch/eight.wav" --threads 2 --simd none --no-progress --overwrite \
    -o "$scratch/parallel.mka" >"$scratch/parallel.log"
"$encoder" "$scratch/eight.wav" --threads 1 --simd none --no-progress --overwrite \
    -o "$scratch/serial.mka" >"$scratch/serial.log"
cmp "$scratch/serial.mka" "$scratch/parallel.mka"
# Raw DCA must also contain aligned core and XLL packets, with no muxer pre-roll.
"$encoder" "$scratch/eight.wav" --threads 1 --simd none --no-progress --overwrite \
    -o "$scratch/raw.dts" >"$scratch/raw.log"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/raw.dts" -f s32le -c:a pcm_s32le -y "$scratch/raw.pcm"
cmp "$scratch/eight.pcm" "$scratch/raw.pcm"
echo 'PASS 7.1 aligned four side policies, forced core, thread determinism, and raw DCA'
