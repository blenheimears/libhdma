#!/bin/sh
# Independent FFmpeg DECODER and ffprobe checks; never used during encoding.
# Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
encoder=$1
ffmpeg=$2
ffprobe=$3
fixture=$4
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for config in '1 48000 16 1031' '2 44100 24 1073' '4 48000 16 1139' '5 48000 24 1129' '6 48000 16 1203'; do
    set -- $config
    channels=$1 rate=$2 depth=$3 samples=$4
    case $channels in 1|2) bitrate=1411200;; *) bitrate=1536000;; esac
    source="$scratch/source.wav"
    "$fixture" "$source" "$channels" "$rate" "$depth" "$samples"
    for fmt in dts mka; do
        output="$scratch/output.$fmt"
        rm -f "$output"
        "$encoder" encode "$source" -o "$output" --lossy --bitrate "$bitrate" > /dev/null
        # Strict independent decode must succeed and preserve channels,
        # rate, number of full 512-sample DCA core frames.
        if [ "$fmt" = dts ]; then inputformat="-f dts"; else inputformat=""; fi
        codec=$("$ffprobe" -v error $inputformat -select_streams a:0 -show_entries stream=codec_name -of default=nk=1:nw=1 "$output")
        [ "$codec" = dts ] || { echo 'Not DCA' >&2; exit 1; }
        observed_channels=$("$ffprobe" -v error $inputformat -select_streams a:0 -show_entries stream=channels -of default=nk=1:nw=1 "$output")
        observed_rate=$("$ffprobe" -v error $inputformat -select_streams a:0 -show_entries stream=sample_rate -of default=nk=1:nw=1 "$output")
        [ "$observed_channels" = "$channels" ] && [ "$observed_rate" = "$rate" ] || {
            echo "Mismatch $channels/$rate -> $observed_channels/$observed_rate ($fmt)" >&2; exit 1;
        }
        "$ffmpeg" -v error -xerror $inputformat -i "$output" -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/decoded.pcm"
        bytes=$(wc -c < "$scratch/decoded.pcm" | tr -d ' ')
        expected=$(( ((samples+511)/512)*512*channels*4 ))
        [ "$bytes" -eq "$expected" ] || {
            echo "PCM frame mismatch: got $bytes expected $expected ($fmt/$channels)" >&2; exit 1;
        }
        if [ "$fmt" = mka ]; then
            # DCA frames survive Matroska demuxing without byte changes.
            "$ffmpeg" -v error -xerror -i "$output" -map 0:a:0 -c:a copy -f dts -y "$scratch/demuxed.dts"
            cmp "$scratch/demuxed.dts" "$scratch/output.dts" || {
                echo 'Matroska did not preserve core packet bytes' >&2; exit 1;
            }
            n=$("$ffprobe" -v error -show_entries stream=index -of csv=p=0 "$output" | wc -l)
            [ "$n" -eq 1 ] || { echo "Unexpected track count $n" >&2; exit 1; }
        fi
    done
    printf 'PASS %s channels, %s Hz, PCM%s, %s source frames, raw DCA + MKA\n' "$channels" "$rate" "$depth" "$samples"
done
# Safety: refuse existing targets, equal paths and unsupported lossless request.
"$fixture" "$scratch/source.wav" 2 48000 16 1024
if "$encoder" encode "$scratch/source.wav" -o "$scratch/source.wav" > /dev/null 2>&1; then exit 1; fi
if "$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" > /dev/null 2>&1; then exit 1; fi
if "$encoder" encode "$scratch/source.wav" --mode invalid-mode > /dev/null 2>&1; then exit 1; fi
# --adpcm is tested in the dedicated independent core/XLL ADPCM suite.
# The default writer must create Matroska without an explicit output flag.
"$encoder" encode "$scratch/source.wav" --lossy > /dev/null
[ -s "$scratch/source.mka" ] || { echo 'Missing default .mka output' >&2; exit 1; }
# Explicit replacement works; the source must remain unchanged.
cp "$scratch/source.wav" "$scratch/source.expected.wav"
"$encoder" encode "$scratch/source.wav" -o "$scratch/output.mka" --lossy --overwrite > /dev/null
cmp "$scratch/source.wav" "$scratch/source.expected.wav"
printf 'PASS safety/path/mode/default-MKA/overwrite guard checks\n'
# Multichannel input regression: canonical WAVEFORMATEXTENSIBLE masks and
# FLAC's specified ordering must encode exactly the same core as PCM WAV.
for config in '4 0x603 0x33 2.2' '5 0x607 0x37 5.0' '6 0x60f 0x3f 5.1'; do
    set -- $config
    channels=$1 side_mask=$2 rear_mask=$3 layout=$4
    "$fixture" "$scratch/layout-legacy.wav" "$channels" 48000 24 1027
    "$encoder" encode "$scratch/layout-legacy.wav" -o "$scratch/layout-reference.dts" --bitrate 1536000 --lossy --overwrite > /dev/null
    for mask in "$side_mask" "$rear_mask"; do
        "$fixture" "$scratch/layout-ext.wav" "$channels" 48000 24 1027 "$mask"
        "$encoder" encode "$scratch/layout-ext.wav" -o "$scratch/layout-ext.dts" --bitrate 1536000 --lossy --overwrite > /dev/null
        cmp "$scratch/layout-ext.dts" "$scratch/layout-reference.dts" || {
            echo "Speaker order changed for WAVEX $channels $mask" >&2; exit 1;
        }
        "$encoder" encode "$scratch/layout-ext.wav" -o "$scratch/layout-ext.mka" --bitrate 1536000 --lossy --overwrite > /dev/null
        "$ffmpeg" -v error -xerror -i "$scratch/layout-ext.mka" -map 0:a:0 -f s32le -c:a pcm_s32le -y "$scratch/layout-decoded.pcm"
        observed=$("$ffprobe" -v error -select_streams a:0 -show_entries stream=channels -of default=nw=1:nk=1 "$scratch/layout-ext.mka")
        [ "$observed" -eq "$channels" ] || { echo "Wrong WAVEX channel count" >&2; exit 1; }
        [ "$(wc -c < "$scratch/layout-decoded.pcm")" -eq "$((1536*channels*4))" ] || {
            echo "Wrong WAVEX decoded frame count" >&2; exit 1;
        }
    done
    # Channel-layout override cannot silently change the channel count.
    case $channels in 4) wrong=5.1;; *) wrong=2.2;; esac
    if "$encoder" encode "$scratch/layout-ext.wav" -o "$scratch/wrong.dts" --channel-layout "$wrong" --bitrate 1536000 --lossy >/dev/null 2>&1; then
        echo "Invalid layout override accepted" >&2;exit 1
    fi
    printf 'PASS WAVEX masks/decoded output, %s channels (%s)\n' "$channels" "$layout"
done
# An unspecified WAVEX mask requires an explicit, correct layout. It must not
# be turned into guessed speakers by a permissive libsndfile channel map.
"$fixture" "$scratch/undefined.wav" 6 48000 24 1027 0
if "$encoder" encode "$scratch/undefined.wav" -o "$scratch/undefined.dts" --bitrate 1536000 --lossy >/dev/null 2>&1; then
    echo 'Accepted WAVEX with undefined speaker mask' >&2;exit 1
fi
"$encoder" encode "$scratch/undefined.wav" -o "$scratch/undefined.dts" --bitrate 1536000 --lossy --channel-layout 5.1 >/dev/null
cmp "$scratch/undefined.dts" "$scratch/layout-reference.dts"
"$fixture" "$scratch/unsupported.wav" 6 48000 24 1027 0x30f
if "$encoder" encode "$scratch/unsupported.wav" -o "$scratch/unsupported.dts" --bitrate 1536000 --lossy >/dev/null 2>&1; then
    echo 'Accepted non-DCA speaker mask' >&2;exit 1
fi
printf 'PASS undefined and unsupported masks, explicit layout override\n'
# FFmpeg is used here solely for test-fixture conversion (FLAC/AIFF),
# followed by independent DCA decoding. It is not an encoder dependency.
"$fixture" "$scratch/surround.wav" 6 48000 24 1027 0x60f
"$ffmpeg" -v error -xerror -y -i "$scratch/surround.wav" -c:a flac "$scratch/surround.flac"
"$encoder" encode "$scratch/surround.flac" -o "$scratch/flac.dts" --bitrate 1536000 --lossy >/dev/null
cmp "$scratch/flac.dts" "$scratch/layout-reference.dts" || {
    echo 'FLAC multichannel ordering changed' >&2; exit 1;
}
"$ffmpeg" -v error -xerror -y -i "$scratch/surround.wav" -c:a pcm_s24be "$scratch/surround.aiff"
if "$encoder" encode "$scratch/surround.aiff" -o "$scratch/aiff.dts" --bitrate 1536000 --lossy >/dev/null 2>&1; then
    echo 'Ambiguous AIFF was accepted without explicit layout' >&2;exit 1
fi
"$encoder" encode "$scratch/surround.aiff" -o "$scratch/aiff.dts" --bitrate 1536000 --lossy --channel-layout 5.1 >/dev/null
cmp "$scratch/aiff.dts" "$scratch/layout-reference.dts" || {
    echo 'AIFF explicit layout changed speaker ordering' >&2;exit 1;
}
printf 'PASS multichannel FLAC standard ordering and AIFF explicit layout\n'
