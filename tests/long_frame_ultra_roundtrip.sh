#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Genuine 1024-sample DCA core/XLL headers, bit-exact PCM/padding, ultra modes,
# default same-channel core referencing, and deterministic thread ordering.
set -eu
encoder=$1 ffmpeg=$2 ffprobe=$3 fixture=$4 inspector=$5
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
run_case() {
    channels=$1 depth=$2 length=$3 mask=$4 signal=$5 mode=$6
    "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" "$length" "$mask" "$signal"
    "$encoder" "$scratch/source.wav" --xll-frame-samples 1024 --threads 2 \
        --simd none --no-progress $mode --overwrite -o "$scratch/output.dts" > "$scratch/encode.log"
    count=$(( (length + 1023) / 1024 ))
    "$inspector" "$scratch/output.dts" 1024 "$count"
    "$encoder" "$scratch/source.wav" --xll-frame-samples 1024 --threads 2 \
        --simd none --no-progress $mode --overwrite -o "$scratch/output.mka" > "$scratch/encode.log"
    profile=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/output.mka")
    [ "$profile" = 'DTS-HD MA' ] || { echo "Invalid profile: $profile" >&2;return 1; }
    "$ffmpeg" -nostdin -v error -xerror -i "$scratch/source.wav" -f s32le -c:a pcm_s32le -y "$scratch/original.pcm"
    "$ffmpeg" -nostdin -v error -xerror -err_detect explode+crccheck -i "$scratch/output.mka" -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
    cmp "$scratch/original.pcm" "$scratch/decoded.pcm"
    printf 'PASS long frame bit-exact: channels=%s depth=%s length=%s mode=%s\n' "$channels" "$depth" "$length" "${mode:-default}"
}
run_case 2 16 1 0x3 silence ''
run_case 2 24 1025 0x3 periodic10 ''
run_case 6 16 1023 0x3f silence ''
run_case 6 24 2050 0x3f correlated ''
run_case 8 24 2050 0x63f musical ''
run_case 8 16 1024 0x63f extremes ''
run_case 2 24 1025 0x3 lsbnoise '--ultra --xll-reference full'
# Dedicated forced-reference case uses non-overflowing 24-bit input.
run_case 2 24 1025 0x3 smooth '--xll-reference core'
# Thread count must not alter the final bitstream; same source and settings.
"$encoder" "$scratch/source.wav" --xll-frame-samples 1024 --threads 1 \
    --simd none --no-progress --overwrite -o "$scratch/single.mka" >/dev/null
"$encoder" "$scratch/source.wav" --xll-frame-samples 1024 --threads 3 \
    --simd none --no-progress --overwrite -o "$scratch/multi.mka" >/dev/null
cmp "$scratch/single.mka" "$scratch/multi.mka"
echo 'PASS 1024-sample threaded byte-for-byte determinism'
