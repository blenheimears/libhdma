#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Require real mixed reference signalling, several independently selected XLL grids and independent
# lossless decoding of each case (including core-residual overflow in 5.1).
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
inspector=$5
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for signal in musical extremes silence smooth; do
    "$fixture" "$scratch/source.wav" 6 48000 24 20480 "$signal"
    "$encoder" encode "$scratch/source.wav" --prediction-order 15 --bitrate 1411200 \
        --xll-reference auto -o "$scratch/output.mka" --overwrite >"$scratch/log"
    case "$signal" in
        musical)
            # Reference-compatible fixed-order policy changes the per-frame
            # size winner for this fixture: the stream still exercises varied
            # segment grids, but mixed core/full reference is no longer the
            # smallest representation for this particular signal.
            "$inspector" "$scratch/output.mka" varied
            "$inspector" "$scratch/output.mka" 2
            "$inspector" "$scratch/output.mka" 3
            ;;
        extremes)
            "$inspector" "$scratch/output.mka" mixed
            "$inspector" "$scratch/output.mka" 1
            ;;
        silence) "$inspector" "$scratch/output.mka" 1;;
        smooth) "$inspector" "$scratch/output.mka" 4;;
    esac
    profile=$("$ffprobe" -v error -select_streams a:0 \
        -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.mka")
    [ "$profile" = 'DTS-HD MA' ] || { echo "Unexpected profile: $profile" >&2;exit 1; }
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
        -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/output.mka" -map 0:a:0 -f s32le -c:a pcm_s32le \
        -y "$scratch/decoded.pcm"
    cmp "$scratch/reference.pcm" "$scratch/decoded.pcm" || {
        echo "Mixed reference or XLL grid corrupted PCM: $signal" >&2;exit 1;
    }
    printf 'PASS %s: bit-exact PCM, valid XLL reference/grid signalling\n' "$signal"
done
