#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Test decoder fidelity AND actual bitstream order, including both worker modes.
set -eu
encoder=$1 ffmpeg=$2 fixture=$3 inspect=$4
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$fixture" "$tmp/source.wav" 6 48000 24 2048 periodic10
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$tmp/source.wav" \
    -f s24le -acodec pcm_s24le -y "$tmp/source.pcm"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$tmp/source.wav" \
    -f s32le -acodec pcm_s32le -y "$tmp/reference.pcm"
for option in default explicit10 alias10 order0 order3 order15 longframe; do
    set --
    case "$option" in
        default) bound=10;;
        explicit10) set -- --prediction-order 10;bound=10;;
        alias10) set -- --xll-max-prediction-order 10;bound=10;;
        order0) set -- --prediction-order 0;bound=0;;
        order3) set -- --prediction-order 3;bound=10;;
        order15) set -- --prediction-order 15;bound=15;;
        longframe) set -- --prediction-order 10 --xll-frame-samples 1024;bound=10;;
    esac
    "$encoder" "$tmp/source.pcm" --raw-pcm --sample-rate 48000 --channels 6 \
       --bit-depth 24 --channel-layout 5.1 --lossless --xll-reference full \
       --threads 4 --no-progress "$@" -o "$tmp/$option.dts" >/dev/null 2>"$tmp/$option.log"
    "$inspect" "$tmp/$option.dts" "limit$bound"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$tmp/$option.dts" -f s32le -acodec pcm_s32le -y "$tmp/decoded.pcm"
    cmp "$tmp/reference.pcm" "$tmp/decoded.pcm"
    echo "PASS $option: XLL order <= $bound, exact decoded 5.1 PCM"
done
cmp "$tmp/default.dts" "$tmp/explicit10.dts"
cmp "$tmp/default.dts" "$tmp/alias10.dts"
# Changing worker count must not change the chosen order or encoded bytes.
"$encoder" "$tmp/source.pcm" --raw-pcm --sample-rate 48000 --channels 6 \
    --bit-depth 24 --channel-layout 5.1 --lossless --xll-reference full \
    --prediction-order 0 --threads 1 --no-progress -o "$tmp/single.dts" >/dev/null 2>"$tmp/single.log"
cmp "$tmp/order0.dts" "$tmp/single.dts"
for invalid in 16 -1 abc 2.5; do
    if "$encoder" "$tmp/source.pcm" --raw-pcm --sample-rate 48000 --channels 6 \
        --bit-depth 24 --channel-layout 5.1 --prediction-order "$invalid" \
        --no-progress -o "$tmp/invalid.dts" >/dev/null 2>"$tmp/invalid.log"; then
        echo "invalid prediction order accepted: $invalid" >&2; exit 1
    fi
    grep -q -- '--prediction-order must be an integer from 0 to 15' "$tmp/invalid.log"
    test ! -e "$tmp/invalid.dts"
done
if "$encoder" "$tmp/source.pcm" --prediction-order >/dev/null 2>"$tmp/missing.log"; then
    echo 'missing predictor option value accepted' >&2; exit 1
fi
printf '%s\n' 'PASS prediction cap, validation and thread determinism'
