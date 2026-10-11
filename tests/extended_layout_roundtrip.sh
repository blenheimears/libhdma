#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Check downmixed core and full-channel XLL independently with FFmpeg.
set -eu
encoder=$1 ffmpeg=$2 ffprobe=$3 fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for channels in 3 7 8; do
    case "$channels" in
        3) layout=3.0; mask=0x7; core=3 ;;
        7) layout=7.0; mask=0x637; core=5 ;;
        8) layout=7.1; mask=0x63f; core=6 ;;
    esac
    for depth in 16 24; do
        for length in 1 1173; do
            "$fixture" "$scratch/original.wav" "$channels" 48000 "$depth" "$length" "$mask"
            # The primary CLI syntax requires no `encode` keyword.
            "$encoder" "$scratch/original.wav" -o "$scratch/lossless.mka" \
                --threads 1 --simd none --overwrite --no-progress > "$scratch/status"
            full=$($ffprobe -v error -select_streams a:0 -show_entries stream=channels \
                -of default=nw=1:nk=1 "$scratch/lossless.mka")
            profile=$($ffprobe -v error -select_streams a:0 -show_entries stream=profile \
                -of default=nw=1:nk=1 "$scratch/lossless.mka")
            [ "$full" -eq "$channels" ] && [ "$profile" = 'DTS-HD MA' ] || {
                echo "XLL channel/profile mismatch $channels/$depth: $full $profile" >&2; exit 1;
            }
            format=s32le
            codec=pcm_s32le
            if [ "$depth" -eq 16 ]; then format=s16le; codec=pcm_s16le; fi
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/original.wav" \
                -f "$format" -c:a "$codec" -y "$scratch/original.pcm"
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
                -i "$scratch/lossless.mka" -f "$format" -c:a "$codec" \
                -y "$scratch/reconstructed.pcm"
            cmp "$scratch/original.pcm" "$scratch/reconstructed.pcm" || {
                echo "Full-channel lossless mismatch $channels/$depth/$length" >&2;exit 1;
            }
            [ "$(wc -c < "$scratch/reconstructed.pcm")" -eq "$((length*channels*(depth==16?2:4)))" ] || exit 1
            # Core-only decode from HD MA must use the downmixed speaker count.
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -core_only 1 \
                -i "$scratch/lossless.mka" -f s32le -c:a pcm_s32le \
                -y "$scratch/core.pcm"
            [ "$(wc -c < "$scratch/core.pcm")" -eq "$((length*core*4))" ] || {
                echo "Invalid embedded core size $channels/$depth/$length" >&2;exit 1;
            }
            "$encoder" "$scratch/original.wav" -o "$scratch/lossy.mka" \
                --lossy --threads 1 --overwrite --no-progress > "$scratch/lossy.status"
            actual=$($ffprobe -v error -select_streams a:0 -show_entries stream=channels \
                -of default=nw=1:nk=1 "$scratch/lossy.mka")
            [ "$actual" -eq "$core" ] || {
                echo "Lossy MKA metadata must report $core channels, got $actual" >&2;exit 1;
            }
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/lossy.mka" \
                -f s32le -c:a pcm_s32le -y "$scratch/lossy.pcm"
            [ "$(wc -c < "$scratch/lossy.pcm")" -eq "$(( ((length+511)/512)*512*core*4 ))" ] || {
                echo "Invalid lossy DCA core PCM $channels/$depth/$length" >&2;exit 1;
            }
            if [ "$channels" -gt 6 ]; then
                grep -q "Core downmix: $channels-channel source -> $core-channel DCA core" "$scratch/status"
                grep -q 'XLL preserves all original channels' "$scratch/status"
                # Forced residuals may legitimately overflow on unbounded
                # random PCM. The dedicated 7.x suite tests forced residuals
                # with range-safe signals and inspects their actual masks.
                "$encoder" "$scratch/original.wav" -o "$scratch/forced-full.mka" \
                    --xll-reference full --threads 1 --simd none --overwrite --no-progress >/dev/null
                # The reference-compatible 24-bit 7.1 hierarchy is chosen for
                # receiver interoperability, not as a per-frame size minimum; it
                # may be larger than the explicit single-set full-PCM diagnostic.
                if [ "$channels" -ne 8 ] || [ "$depth" -ne 24 ]; then
                    [ "$(wc -c < "$scratch/lossless.mka")" -le "$(wc -c < "$scratch/forced-full.mka")" ] || {
                        echo 'AUTO XLL exceeded full-PCM fallback' >&2;exit 1;
                    }
                fi
            fi
        done
        echo "PASS $layout input: $depth-bit, exact $channels-channel XLL, $core-channel core"
    done
done
# Test 8-channel canonical FLAC's channel ordering, with exact PCM comparison.
"$fixture" "$scratch/original.wav" 8 48000 24 513 0x63f
"$ffmpeg" -hide_banner -nostdin -v error -xerror -y -i "$scratch/original.wav" -c:a flac "$scratch/eight.flac"
"$encoder" "$scratch/eight.flac" -o "$scratch/flac.mka" --threads 1 --overwrite --no-progress >/dev/null
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/eight.flac" -f s32le -c:a pcm_s32le -y "$scratch/expected.pcm"
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/flac.mka" -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
cmp "$scratch/expected.pcm" "$scratch/decoded.pcm"
echo 'PASS FLAC 7.1 canonical order bit-exact'
# Unsupported/ambiguous seven-channel FLAC is rejected even with an override.
"$fixture" "$scratch/original.wav" 7 48000 24 513 0x637
"$ffmpeg" -hide_banner -nostdin -v error -xerror -y -i "$scratch/original.wav" -c:a flac "$scratch/seven.flac"
if "$encoder" "$scratch/seven.flac" -o "$scratch/invalid.mka" --channel-layout 7.0 \
    --overwrite --no-progress > /dev/null 2>&1; then
    echo 'Seven-channel FLAC reordered into wrong speakers' >&2; exit 1
fi
echo 'PASS unsupported FLAC 6.1 order rejected safely'
