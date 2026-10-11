#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project.
# SPDX-License-Identifier: LGPL-2.1-or-later.
# Verify 7.0/7.1 speaker-aware core residual flags, all side policies, and
# untouched original PCM for both 16-bit and 24-bit input. FFmpeg is test-only.
set -eu
encoder=$1 ffmpeg=$2 ffprobe=$3 fixture=$4 inspector=$5
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for channels in 7 8; do
    case "$channels" in 7) mask=0x637; core=5;; 8) mask=0x63f; core=6;; esac
    for depth in 16 24; do
        "$fixture" "$scratch/source.wav" "$channels" 48000 "$depth" 513 "$mask" musical
        case "$depth" in 16) format=s16le; codec=pcm_s16le;; 24)format=s32le; codec=pcm_s32le;; esac
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/source.wav" \
            -f "$format" -c:a "$codec" -y "$scratch/source.pcm"
        for policy in both front rear mute; do
            "$encoder" "$scratch/source.wav" --core-side "$policy" \
                --xll-reference core --threads 1 --simd none --no-progress \
                -o "$scratch/core-$policy.mka" --overwrite > "$scratch/log"
            grep -q "side policy=$policy" "$scratch/log"
            # FFmpeg must actually recognize the XLL profile and source channels.
            actual=$($ffprobe -v error -select_streams a:0 -show_entries stream=channels \
                -of default=nw=1:nk=1 "$scratch/core-$policy.mka")
            profile=$($ffprobe -v error -select_streams a:0 -show_entries stream=profile \
                -of default=nw=1:nk=1 "$scratch/core-$policy.mka")
            [ "$actual" -eq "$channels" ] && [ "$profile" = 'DTS-HD MA' ]
            if [ "$channels" -eq 8 ] && [ "$depth" -eq 24 ]; then
                # Normal BOTH uses the stable receiver-compatible hierarchy.
                # Alternate diagnostic core downmixes stay aligned but use the
                # single-set representation because the hierarchy matrix is
                # specifically defined for BOTH.
                if [ "$policy" = both ]; then
                    "$inspector" "$scratch/core-$policy.mka" "$channels" hier-stable
                else
                    "$inspector" "$scratch/core-$policy.mka" "$channels" single
                fi
            else
                "$inspector" "$scratch/core-$policy.mka" "$channels"
            fi
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
                -i "$scratch/core-$policy.mka" -f "$format" -c:a "$codec" \
                -y "$scratch/decoded.pcm"
            cmp "$scratch/source.pcm" "$scratch/decoded.pcm"
            # The separately decodable core must contain 5.0/5.1, never
            # the original 7/8 channels or a mislabeled Matroska count.
            "$ffmpeg" -hide_banner -nostdin -v error -xerror -core_only 1 \
                -i "$scratch/core-$policy.mka" -f s32le -c:a pcm_s32le \
                -y "$scratch/lossy-$policy.pcm"
            [ "$(wc -c < "$scratch/lossy-$policy.pcm")" -eq "$((513*core*4))" ]
            echo "PASS $channels channels $depth-bit: forced 5.x core reference policy=$policy, exact PCM"
        done
        # Default policy must be exactly BOTH (including core encoding).
        "$encoder" "$scratch/source.wav" --xll-reference core --threads 1 \
            --simd none --no-progress -o "$scratch/default.mka" --overwrite >/dev/null
        cmp "$scratch/default.mka" "$scratch/core-both.mka"
        # Every policy must make a real change to the lossy core when the
        # fixture's side channels contain audio; none affects lossless PCM.
        ! cmp -s "$scratch/lossy-mute.pcm" "$scratch/lossy-front.pcm"
        ! cmp -s "$scratch/lossy-mute.pcm" "$scratch/lossy-rear.pcm"
        ! cmp -s "$scratch/lossy-front.pcm" "$scratch/lossy-rear.pcm"
        # AUTO may select full or core residual per speaker, but must never
        # exceed its full-PCM candidate when core and source are identical.
        "$encoder" "$scratch/source.wav" --core-side both \
            --xll-reference auto --threads 1 --simd none --no-progress \
            -o "$scratch/auto.mka" --overwrite >/dev/null
        "$encoder" "$scratch/source.wav" --core-side both \
            --xll-reference full --threads 1 --simd none --no-progress \
            -o "$scratch/full.mka" --overwrite >/dev/null
        if [ "$channels" -eq 8 ] && [ "$depth" -eq 24 ]; then
            # The receiver-compatible two-set hierarchy has fixed structural
            # overhead and may be larger than the old single-set full-PCM form
            # on tiny fixtures. Compare AUTO against the same hierarchy instead.
            "$encoder" "$scratch/source.wav" --core-side both \
                --xll-reference full --xll-7x-hierarchy on --threads 1 --simd none --no-progress \
                -o "$scratch/full-hier.mka" --overwrite >/dev/null
            [ "$(wc -c < "$scratch/auto.mka")" -le "$(wc -c < "$scratch/full-hier.mka")" ]
        else
            [ "$(wc -c < "$scratch/auto.mka")" -le "$(wc -c < "$scratch/full.mka")" ]
        fi
        "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
            -i "$scratch/auto.mka" -f "$format" -c:a "$codec" \
            -y "$scratch/auto.pcm"
        cmp "$scratch/source.pcm" "$scratch/auto.pcm"
    done
done

# 24-bit/512-sample 7.1 gets explicit topology controls. OFF forces one
# stable eight-channel set. ON keeps the receiver-compatible two-set hierarchy
# and uses aligned core residuals after the self-contained startup frame. AUTO
# remains the legacy per-frame topology comparison diagnostic.
"$fixture" "$scratch/topology.wav" 8 48000 24 8192 0x63f musical
"$ffmpeg" -hide_banner -nostdin -v error -xerror -i "$scratch/topology.wav" \
    -f s32le -c:a pcm_s32le -y "$scratch/topology.pcm"
for topology in off on auto; do
    "$encoder" "$scratch/topology.wav" --xll-reference core --xll-7x-hierarchy "$topology" \
        --threads 1 --simd none --no-progress --overwrite -o "$scratch/topology-$topology.mka" \
        >"$scratch/topology-$topology.log"
    case "$topology" in
        off) "$inspector" "$scratch/topology-$topology.mka" 8 single ;;
        on) "$inspector" "$scratch/topology-$topology.mka" 8 hier-stable ;;
        auto) "$inspector" "$scratch/topology-$topology.mka" 8 ;;
    esac
    "$ffmpeg" -hide_banner -nostdin -v error -xerror -err_detect explode+crccheck \
        -i "$scratch/topology-$topology.mka" -f s32le -c:a pcm_s32le \
        -y "$scratch/topology-$topology.pcm"
    cmp "$scratch/topology.pcm" "$scratch/topology-$topology.pcm"
done
grep -q '7.1 XLL topology: single-set (explicit diagnostic override)' "$scratch/topology-off.log"
grep -q '7.1 XLL topology: embedded 5.1 primary + Lsr/Rsr extension (stable)' "$scratch/topology-on.log"
grep -q '7.1 XLL topology: legacy per-frame auto-switching (diagnostic)' "$scratch/topology-auto.log"
if "$encoder" "$scratch/topology.wav" --xll-7x-hierarchy nonsense --no-progress \
    -o "$scratch/invalid-topology.mka" --overwrite >/dev/null 2>&1; then exit 1; fi

# Do not silently accept policies for sources without independently coded
# side channels, or malformed/duplicate options.
"$fixture" "$scratch/stereo.wav" 2 48000 16 513
if "$encoder" "$scratch/stereo.wav" --core-side mute -o "$scratch/invalid.mka" --overwrite --no-progress >/dev/null 2>&1;then exit 1;fi
for policy in foo '' BOTH;do
    if "$encoder" "$scratch/stereo.wav" --core-side "$policy" -o "$scratch/invalid.mka" --overwrite --no-progress >/dev/null 2>&1;then exit 1;fi
done
if "$encoder" "$scratch/stereo.wav" --core-side both --core-side mute -o "$scratch/invalid.mka" --overwrite --no-progress >/dev/null 2>&1;then exit 1;fi
echo 'PASS 7.x stable hardware topology, explicit hierarchy, references, side policies, decoder and errors'
