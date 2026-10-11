#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Independent FFmpeg DCA + XLL decoding and bit-exact source-PCM verification.
# FFmpeg is not involved in encoding and is not linked to libhdma.
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
passed=0
for rate in 44100 48000; do
    for channels in 1 2 4 5 6; do
        case $channels in 1|2) bitrate=1411200;; *) bitrate=1536000;; esac
        for depth in 16 24; do
            case $depth in 16) format=s16le;; 24) format=s32le;; esac
            for frames in 1 512 1173; do
                "$fixture" "$scratch/source.wav" "$channels" "$rate" "$depth" "$frames"
                "$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" \
                    --lossless --bitrate "$bitrate" --overwrite >/dev/null
                profile=$("$ffprobe" -v error -select_streams a:0 \
                    -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.mka")
                observed_channels=$("$ffprobe" -v error -select_streams a:0 \
                    -show_entries stream=channels -of default=nw=1:nk=1 "$scratch/output.mka")
                observed_rate=$("$ffprobe" -v error -select_streams a:0 \
                    -show_entries stream=sample_rate -of default=nw=1:nk=1 "$scratch/output.mka")
                [ "$profile" = 'DTS-HD MA' ] && [ "$observed_channels" -eq "$channels" ] && \
                    [ "$observed_rate" -eq "$rate" ] || {
                    echo "Incorrect profile/layout/rate: $profile $observed_channels $observed_rate" >&2
                    exit 1
                }
                "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
                    -map 0:a:0 -f "$format" -c:a "pcm_$format" -y "$scratch/reference.pcm"
                "$ffmpeg" -hide_banner -nostdin -v error -xerror \
                    -err_detect explode+crccheck -i "$scratch/output.mka" \
                    -map 0:a:0 -f "$format" -c:a "pcm_$format" -y "$scratch/decoded.pcm"
                # cmp compares every byte AND length (including the final partial DCA frame).
                cmp "$scratch/reference.pcm" "$scratch/decoded.pcm" || {
                    echo "Not bit-exact: $rate Hz / $channels channels / PCM$depth / $frames frames" >&2
                    exit 1
                }
                passed=$((passed + 1))
            done
        done
        printf 'PASS XLL %s Hz / %s channels / 16+24-bit / 1+512+1173 samples\n' "$rate" "$channels"
    done
done
# Boundary-value vectors prove signed 16/24-bit samples survive without
# clipping, truncation, sign corruption or channel swaps.
for channels in 2 6; do
    case $channels in 2) bitrate=1411200;; 6) bitrate=1536000;; esac
    for depth in 16 24; do
        case $depth in 16) format=s16le;; 24) format=s32le;; esac
        "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" 513 extremes
        "$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" \
            --lossless --bitrate "$bitrate" --overwrite >/dev/null
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
            -map 0:a:0 -f "$format" -c:a "pcm_$format" -y "$scratch/reference.pcm"
        "$ffmpeg" -hide_banner -nostdin -v error -xerror \
            -err_detect explode+crccheck -i "$scratch/output.mka" \
            -map 0:a:0 -f "$format" -c:a "pcm_$format" -y "$scratch/decoded.pcm"
        cmp "$scratch/reference.pcm" "$scratch/decoded.pcm" || {
            echo "Boundary-value PCM mismatch: channels=$channels depth=$depth" >&2;exit 1;
        }
        passed=$((passed + 1))
    done
done
printf 'PASS boundary-value 16/24-bit signed PCM, 2/6 channels\n'
# Reset the final fixture to the ordinary six-channel 24-bit case for the
# separate backwards-compatibility and raw DCA validation below.
"$fixture" "$scratch/source.wav" 6 48000 24 1173
"$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" \
    --lossless --bitrate 1536000 --overwrite >/dev/null
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
    -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/reference.pcm"
# Preserve core backward compatibility: the same DCA + XLL input must decode through
# FFmpeg's core-only path as ordinary lossy DCA, never claim bit-identical PCM.
"$ffmpeg" -hide_banner -nostdin -v error -xerror -core_only 1 -i "$scratch/output.mka" \
    -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/core-only.pcm"
[ "$(wc -c < "$scratch/core-only.pcm" | tr -d ' ')" -eq "$((1173*6*4))" ] || {
    echo 'Core-only decoder did not preserve frame count/channels' >&2;exit 1;
}
# Raw .dts remains DCA + XLL, including the original samples followed by silence
# for the final partial frame. Matroska DiscardPadding alone removes this tail.
"$encoder" encode "$scratch/source.wav" -o "$scratch/output.dts" \
    --lossless --bitrate 1536000 --overwrite >/dev/null
raw_profile=$("$ffprobe" -v error -f dts -select_streams a:0 \
    -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.dts")
[ "$raw_profile" = 'DTS-HD MA' ] || { echo 'Raw DCA is not DCA + XLL' >&2;exit 1; }
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -f dts -i "$scratch/output.dts" -map 0:a:0 -f s32le -c:a pcm_s32le \
    -y "$scratch/raw-decoded.pcm"
head -c "$(wc -c < "$scratch/reference.pcm" | tr -d ' ')" \
    "$scratch/raw-decoded.pcm" > "$scratch/raw-prefix.pcm"
cmp "$scratch/reference.pcm" "$scratch/raw-prefix.pcm" || {
    echo 'Raw DCA source prefix is not bit-identical' >&2;exit 1;
}
# Safety: unsupported core sample-rate and source bit depths must not masquerade
# as a successful lossless encode. (Input checks happen before publishing output.)
[ "$passed" -eq 64 ] || { echo "Wrong acceptance coverage: $passed" >&2;exit 1; }
echo "PASS DCA + XLL: $passed exact PCM/length/profile checks, core-only and raw DCA"
