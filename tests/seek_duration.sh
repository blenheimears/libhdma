#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
structure=$5
compare=$6
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$fixture" "$scratch/source.wav" 2 48000 24 100000 smooth
"$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" > /dev/null
"$structure" "$scratch/output.mka" 100000 48000
profile=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.mka")
[ "$profile" = 'DTS-HD MA' ] || { echo 'Default mode is not lossless DCA + XLL' >&2;exit 1; }
duration=$("$ffprobe" -v error -show_entries format=duration -of default=nw=1:nk=1 "$scratch/output.mka")
awk -v got="$duration" 'BEGIN { diff=got-100000/48000; if(diff<0)diff=-diff; exit(diff < .000002 ? 0 : 1) }' || {
    echo "Wrong output duration $duration" >&2;exit 1;
}
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/output.mka" -f s32le -c:a pcm_s32le -y "$scratch/full.pcm"
cmp "$scratch/source.pcm" "$scratch/full.pcm" || {
    echo 'The default lossless mode changed PCM samples or total length' >&2;exit 1;
}
for seconds in 0.75 1.50; do
    case "$seconds" in 0.75) start=36000;; 1.50) start=72000;; esac
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -ss "$seconds" \
        -i "$scratch/output.mka" -t 0.10 -f s32le -c:a pcm_s32le -y "$scratch/seek.pcm"
    "$compare" "$scratch/source.pcm" "$scratch/seek.pcm" "$start"
done
# The formerly uncompressed PCM extension was larger by more than 20% on
# this deterministic smooth PCM fixture; the core is included in both sizes.
# A fixed output ceiling guards accidental return to raw PCM in XLL.
size=$(wc -c < "$scratch/output.mka" | tr -d ' ')
[ "$size" -lt 810000 ] || { echo "Compressed XLL size regression: $size" >&2;exit 1; }
# Changing the core bitrate must retain losslessness and actually change the
# lossy core. The independent DCA decoder reports the lower core rate.
"$encoder" encode "$scratch/source.wav" -o "$scratch/lower.mka" --bitrate 768000 > /dev/null
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/lower.mka" -f s32le -c:a pcm_s32le -y "$scratch/lower.pcm"
cmp "$scratch/source.pcm" "$scratch/lower.pcm" || {
    echo 'Custom core bitrate broke lossless reconstruction' >&2;exit 1;
}
"$encoder" encode "$scratch/source.wav" -o "$scratch/core.mka" --lossy --bitrate 768000 > /dev/null
core_profile=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/core.mka")
core_rate=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=bit_rate -of default=nw=1:nk=1 "$scratch/core.mka")
[ "$core_profile" = DTS ] && [ "$core_rate" -eq 768000 ] || {
    echo "Lossy-only mode/core bitrate incorrect: $core_profile/$core_rate" >&2;exit 1;
}
printf 'PASS duration=%s seconds, default lossless mode, indexed seeking, compressed size=%s bytes\n' "$duration" "$size"
