#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Independently verify that XLL actually references the core while preserving
# exact original PCM content, channel ordering and final sample count.
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
passed=0
for rate in 48000 44100; do
    for channels in 1 2 4 5 6; do
        # Test all speaker groups at 48k and the widely used mono/stereo 44.1k subset.
        [ "$rate" = 48000 ] || [ "$channels" -le 2 ] || continue
        case $channels in 1|2) bitrate=1411200;; *) bitrate=1536000;; esac
        for depth in 16 24; do
            case $depth in 16) format=s16le;; 24) format=s32le;; esac
            for count in 1024 1173; do
                "$fixture" "$scratch/input.wav" "$channels" "$rate" "$depth" "$count" smooth
                "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
                    -map 0:a:0 -f "$format" -c:a "pcm_$format" -y "$scratch/source.pcm"
                "$encoder" encode "$scratch/input.wav" -o "$scratch/output.mka" \
                    --xll-reference core --bitrate "$bitrate" --overwrite >"$scratch/encode.log"
                grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames, 1 full-PCM frames \(policy=core\)' "$scratch/encode.log" || {
                    echo "Core residual was not actually used: $channels/$rate/$depth/$count" >&2;exit 1;
                }
                profile=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=profile \
                    -of default=nw=1:nk=1 "$scratch/output.mka")
                [ "$profile" = "DTS-HD MA" ] || { echo "Wrong DCA + XLL profile: $profile" >&2;exit 1; }
                "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
                    -i "$scratch/output.mka" -map 0:a:0 -f "$format" -c:a "pcm_$format" \
                    -y "$scratch/decoded.pcm"
                cmp "$scratch/source.pcm" "$scratch/decoded.pcm" || {
                    echo "Core-reference PCM mismatch: $channels/$rate/$depth/$count" >&2;exit 1;
                }
                passed=$((passed+1))
            done
        done
    done
done
[ "$passed" -eq 28 ] || { echo "Expected 28 tests, got $passed" >&2;exit 1; }
# Realistically tonal data exercises both AUTO branches within one encode.
# A regression which never selects the new core-referenced path must fail.
"$fixture" "$scratch/input.wav" 6 48000 24 20480 musical
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
    -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"
"$encoder" encode "$scratch/input.wav" -o "$scratch/output.mka" \
    --bitrate 1411200 --xll-reference auto --overwrite >"$scratch/encode.log"
grep -Eq 'XLL prediction: [1-9][0-9]* core-referenced frames, [1-9][0-9]* full-PCM frames \(policy=auto\)' "$scratch/encode.log" || {
    cat "$scratch/encode.log" >&2
    echo 'AUTO failed to exercise both XLL reference modes' >&2;exit 1;
}
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/output.mka" -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
cmp "$scratch/source.pcm" "$scratch/decoded.pcm" || {
    echo 'Mixed AUTO reference frames changed decoded PCM' >&2;exit 1;
}
# A legal full-PCM request still works, and AUTO must safely fall back if core
# residual exceeds signed PCM range rather than silently clamping/overflowing.
"$fixture" "$scratch/input.wav" 6 48000 24 513 extremes
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/input.wav" \
    -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/source.pcm"
for policy in full auto; do
    "$encoder" encode "$scratch/input.wav" -o "$scratch/output.mka" \
        --xll-reference "$policy" --bitrate 1536000 --overwrite > "$scratch/encode.log"
    grep -Eq 'XLL prediction: 0 core-referenced frames, 2 full-PCM frames' "$scratch/encode.log" || {
        echo "Expected full-PCM safety path for $policy" >&2;exit 1;
    }
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/output.mka" -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
    cmp "$scratch/source.pcm" "$scratch/decoded.pcm" || {
        echo "Safety fallback altered PCM: $policy" >&2;exit 1;
    }
done
# With aligned core reconstruction, the same extreme signal is now legal in
# forced mode after the mandatory self-contained first frame.
"$encoder" "$scratch/input.wav" -o "$scratch/forced-core.mka" \
    --xll-reference core --bitrate 1536000 --overwrite > "$scratch/force.log"
grep -q '1 core-referenced frames, 1 full-PCM frames' "$scratch/force.log"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
    -i "$scratch/forced-core.mka" -map 0:a:0 -f s32le -c:a pcm_s32le \
    -y "$scratch/decoded.pcm"
cmp "$scratch/source.pcm" "$scratch/decoded.pcm"
rm -f "$scratch/should-not-exist.mka"
if "$encoder" encode "$scratch/input.wav" -o "$scratch/should-not-exist.mka" \
        --lossy --xll-reference core >"$scratch/force.log" 2>&1; then
    echo 'Lossy-only mode incorrectly accepted XLL-specific option' >&2;exit 1
fi
[ ! -e "$scratch/should-not-exist.mka" ] || exit 1
printf 'PASS %s forced core-referenced bit-exact FFmpeg round trips; full/auto PCM and overflow safety checks\n' "$passed"
