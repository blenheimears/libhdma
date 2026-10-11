#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Validate partial-frame accounting, completed Matroska size, redirected logs,
# terminal carriage-return refresh and --no-progress in a real encode.
set -eu
encoder=$1
fixture=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Non-multiple of 512 catches accidentally counting final-frame padding.
"$fixture" "$tmp/source.wav" 2 48000 16 2049 smooth
"$encoder" encode "$tmp/source.wav" --threads 2 -o "$tmp/test.mka" \
    >"$tmp/out.log" 2>"$tmp/progress.log"
[ "$(grep -c '^hdmaenc: encoding 100%' "$tmp/progress.log")" -eq 1 ] || {
    echo 'Progress must reach 100% once, after muxer finalization' >&2; exit 1;
}
if LC_ALL=C grep -q "$(printf '\033')" "$tmp/progress.log" || \
   LC_ALL=C grep -q "$(printf '\r')" "$tmp/progress.log"; then
    echo 'Redirected stderr contains terminal-control sequences' >&2; exit 1
fi
# Check every reported non-initial ratio has exactly three fractional digits.
if grep 'ratio=' "$tmp/progress.log" | grep -v 'ratio=---' | \
   grep -Ev 'ratio=[0-9]+\.[0-9][0-9][0-9]$' >/dev/null; then
    echo 'Malformed compression ratio in progress output' >&2; exit 1
fi
# Compare codec bytes with an independently encoded raw elementary stream.
# Comparing the .mka file size would incorrectly include Segment/Cues overhead.
"$encoder" encode "$tmp/source.wav" --threads 1 --no-progress \
    -o "$tmp/reference.dts" > "$tmp/reference.log"
codec_size=$(wc -c <"$tmp/reference.dts" | tr -d ' ')
container_size=$(wc -c <"$tmp/test.mka" | tr -d ' ')
[ "$codec_size" -lt "$container_size" ] || {
    echo 'Expected Matroska overhead in container output' >&2; exit 1;
}
expected=$(awk -v bytes="$codec_size" 'BEGIN { printf "%.3f", bytes / (2049 * 2 * 2) }')
stereo_expected=$expected
if ! grep -q "ratio=$expected$" "$tmp/progress.log"; then
    echo "Incorrect final ratio; expected $expected from raw codec bytes" >&2
    tail -1 "$tmp/progress.log" >&2
    exit 1
fi
if ! grep -q "raw codec: $codec_size bytes; container: $container_size bytes" "$tmp/out.log";then
    echo 'Codec/container byte accounting does not match actual files' >&2;exit 1;
fi
"$encoder" encode "$tmp/source.wav" --ratio-container -o "$tmp/with-container.mka" \
    >"$tmp/container-out.log" 2>"$tmp/container-progress.log"
container_expected=$(awk -v bytes="$container_size" 'BEGIN { printf "%.3f", bytes / (2049 * 2 * 2) }')
grep -q "ratio=$container_expected$" "$tmp/container-progress.log" || {
    echo '--ratio-container did not count completed Matroska bytes' >&2;exit 1;
}
"$encoder" encode "$tmp/source.wav" --ratio-container --ratio-codec \
    -o "$tmp/explicit-codec.mka" >"$tmp/explicit-codec-out.log" 2>"$tmp/explicit-codec-progress.log"
grep -q "ratio=$expected$" "$tmp/explicit-codec-progress.log" || {
    echo '--ratio-codec did not restore raw-codec accounting' >&2;exit 1;
}
# Ensure 24-bit uses three bytes/sample and raw DCA has no Matroska overhead.
"$fixture" "$tmp/24bit.wav" 6 48000 24 513 smooth
"$encoder" encode "$tmp/24bit.wav" --threads 1 -o "$tmp/raw.dts" \
    >"$tmp/rawout.log" 2>"$tmp/rawprogress.log"
size=$(wc -c <"$tmp/raw.dts" | tr -d ' ')
expected=$(awk -v bytes="$size" 'BEGIN { printf "%.3f", bytes / (513 * 6 * 3) }')
grep -q "ratio=$expected$" "$tmp/rawprogress.log" || {
    echo "Raw 24-bit ratio mismatch (expected $expected)" >&2; exit 1;
}
"$encoder" encode "$tmp/source.wav" --no-progress -o "$tmp/silent.mka" \
    >"$tmp/silentout.log" 2>"$tmp/silenterr.log"
if grep -q 'encoding [0-9]' "$tmp/silenterr.log"; then
    echo '--no-progress did not disable status output' >&2; exit 1
fi
"$encoder" encode "$tmp/source.wav" --no-progress --progress -o "$tmp/enabled.mka" \
    >"$tmp/enabledout.log" 2>"$tmp/enablederr.log"
grep -q '^hdmaenc: encoding 100%' "$tmp/enablederr.log" || {
    echo '--progress did not restore status output' >&2; exit 1;
}
# 'script' is test-only; the encoder has no dependency on it. On platforms
# without script the redirected tests above still run.
if command -v script >/dev/null 2>&1; then
    script -q -e -c "'$encoder' encode '$tmp/source.wav' --threads 2 -o '$tmp/terminal.mka'" \
        "$tmp/terminal.log" >/dev/null
    [ "$(grep -c 'Encoding ' "$tmp/terminal.log")" -eq 1 ] || {
        echo 'TTY progress must occupy one physical transcript line' >&2; exit 1;
    }
    if ! LC_ALL=C grep -Fq "$(printf '\033[2K')" "$tmp/terminal.log"; then
        echo 'TTY progress did not replace the existing line' >&2; exit 1
    fi
    [ "$(LC_ALL=C tr -cd '\r' <"$tmp/terminal.log" | wc -c | tr -d ' ')" -ge 2 ] || {
        echo 'TTY progress did not emit carriage returns' >&2; exit 1;
    }
    grep -q "ratio=$stereo_expected" "$tmp/terminal.log" || {
        echo 'TTY final ratio does not match the raw codec stream' >&2; exit 1;
    }
fi
echo 'PASS progress: raw codec default and container opt-in, TTY single-line, 16/24-bit ratios, opt-out'
