#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Verify that parallel optimization preserves the EXACT stream, not just PCM.
set -eu
encoder=$1
ffmpeg=$2
fixture=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$fixture" "$tmp/input.wav" 6 48000 24 16385 smooth
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$tmp/input.wav" \
    -f s32le -c:a pcm_s32le -y "$tmp/original.pcm"
for mode in auto full core; do
    "$encoder" encode "$tmp/input.wav" --bitrate 1536000 \
        --xll-reference "$mode" --threads 1 \
        -o "$tmp/serial.mka" --overwrite > "$tmp/serial.log"
    for threads in 2 4 8 16; do
        "$encoder" encode "$tmp/input.wav" --bitrate 1536000 \
            --xll-reference "$mode" --threads "$threads" \
            -o "$tmp/parallel.mka" --overwrite > "$tmp/parallel.log"
        cmp "$tmp/serial.mka" "$tmp/parallel.mka" || {
            echo "Nondeterministic stream: mode=$mode threads=$threads" >&2;exit 1;
        }
    done
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$tmp/parallel.mka" -f s32le -c:a pcm_s32le -y "$tmp/decoded.pcm"
    cmp "$tmp/original.pcm" "$tmp/decoded.pcm" || {
        echo "PCM mismatch: mode=$mode" >&2;exit 1;
    }
    echo "PASS XLL mode=$mode, threads=1/2/4/8/16: identical MKA + bit-exact PCM"
done
# Short final frame: verify padding and muxer sample count are deterministic.
"$fixture" "$tmp/short.wav" 2 44100 16 513
"$encoder" encode "$tmp/short.wav" --threads 1 -o "$tmp/short-one.mka" >/dev/null
"$encoder" encode "$tmp/short.wav" --threads 8 -o "$tmp/short-eight.mka" >/dev/null
cmp "$tmp/short-one.mka" "$tmp/short-eight.mka"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$tmp/short-eight.mka" \
    -f s16le -c:a pcm_s16le -y "$tmp/short.pcm"
[ "$(wc -c < "$tmp/short.pcm" | tr -d ' ')" -eq 2052 ] || {
    echo 'Incorrect partial final-frame sample count' >&2;exit 1;
}
# Lossy-only raw elementary stream must remain unchanged; no unnecessary jobs.
"$encoder" encode "$tmp/input.wav" --lossy --bitrate 1536000 \
    --threads 1 -o "$tmp/core-one.dts" >/dev/null
"$encoder" encode "$tmp/input.wav" --lossy --bitrate 1536000 \
    --threads 8 -o "$tmp/core-eight.dts" >/dev/null
cmp "$tmp/core-one.dts" "$tmp/core-eight.dts"
for invalid in 0 65 abc -1; do
    if "$encoder" encode "$tmp/input.wav" --threads "$invalid" \
        -o "$tmp/invalid.mka" > /dev/null 2>&1;then
        echo "Invalid thread count accepted: $invalid" >&2;exit 1;
    fi
done
if "$encoder" encode "$tmp/input.wav" --threads >/dev/null 2>&1;then
    echo 'Missing --threads argument accepted' >&2;exit 1
fi
echo 'PASS partial frame, lossy-only, invalid thread arguments'
