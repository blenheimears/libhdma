#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Exercise both NEW syntax branches, and decode every PCM sample independently.
set -eu
encoder=$1
ffmpeg=$2
fixture=$3
inspector=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for feature in highorder hybrid; do
    case $feature in
        highorder) channels=2; depth=16; frames=513; signal=extremes;;
        hybrid) channels=6; depth=24; frames=12288; signal=musical;;
    esac
    "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" "$frames" "$signal"
    "$encoder" encode "$scratch/source.wav" --prediction-order 15 --xll-reference full \
        -o "$scratch/output.mka" --overwrite >/dev/null
    "$inspector" "$scratch/output.mka" "$feature"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
        -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/output.mka" -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
    cmp "$scratch/reference.pcm" "$scratch/decoded.pcm" || {
        echo "PCM mismatch in $feature coding" >&2; exit 1;
    }
    echo "PASS $feature: actual XLL syntax + bit-exact decoded PCM"
done
