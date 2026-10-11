#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Verify that CPU dispatch only changes speed, never packet/PCM data.
set -eu
encoder=$1
ffmpeg=$2
fixture=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$fixture" "$tmp/source.wav" 6 48000 24 1537 smooth
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$tmp/source.wav" \
    -f s32le -c:a pcm_s32le -y "$tmp/orig.pcm"
"$encoder" encode "$tmp/source.wav" --simd none --threads 2 \
    -o "$tmp/none.mka" --no-progress >"$tmp/none.log"
if ! "$encoder" encode "$tmp/source.wav" --simd x86-64-v3 --threads 2 \
    -o "$tmp/v3.mka" --no-progress >"$tmp/v3.log" 2>"$tmp/v3.err"; then
    if grep -q 'SIMD backend unavailable' "$tmp/v3.err";then
        "$encoder" encode "$tmp/source.wav" --simd auto --threads 2 \
            -o "$tmp/auto.mka" --no-progress >"$tmp/auto.log"
        cmp "$tmp/none.mka" "$tmp/auto.mka"
        echo 'PASS SIMD: unsupported CPU/build rejected forced v3; auto falls back to scalar'
        exit 0
    fi
    cat "$tmp/v3.err" >&2;exit 1
fi
cmp "$tmp/none.mka" "$tmp/v3.mka"
"$encoder" encode "$tmp/source.wav" --simd auto --threads 1 \
    -o "$tmp/auto.mka" --no-progress >"$tmp/auto.log"
cmp "$tmp/none.mka" "$tmp/auto.mka"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$tmp/v3.mka" -f s32le -c:a pcm_s32le -y "$tmp/decoded.pcm"
cmp "$tmp/orig.pcm" "$tmp/decoded.pcm"
grep -q 'SIMD backend: x86-64-v3' "$tmp/v3.log"
grep -q 'SIMD backend: none' "$tmp/none.log"
echo 'PASS SIMD: x86-64-v3/none/auto bitstreams identical and FFmpeg PCM bit exact'
