#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
encoder=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Eight 512-sample frames, stereo, 24-bit LE. An actual pipe tests stdin.
dd if=/dev/zero of="$tmp/pcm" bs=6144 count=4 2>/dev/null
cat "$tmp/pcm" | "$encoder" - --raw-pcm --sample-rate 48000 \
  --channels 2 --bit-depth 24 --channel-layout 2.0 --lossless \
  --no-progress --overwrite -o "$tmp/out.dts" >/dev/null 2>"$tmp/log"
test -s "$tmp/out.dts"
# Validate independently if an FFmpeg decoder is installed.
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -v error -y -i "$tmp/out.dts" -f s24le -ac 2 -ar 48000 "$tmp/decoded.pcm"
    head -c 24576 "$tmp/decoded.pcm" > "$tmp/decoded-trimmed.pcm"
    cmp "$tmp/pcm" "$tmp/decoded-trimmed.pcm"
fi
# A fractional PCM sample must be rejected, and no output can be published.
if printf x | "$encoder" - --raw-pcm --sample-rate 48000 \
    --channels 2 --bit-depth 24 --channel-layout 2.0 --lossless \
    --no-progress -o "$tmp/broken.dts" >/dev/null 2>"$tmp/failure"; then
    echo 'truncated PCM frame unexpectedly accepted' >&2
    exit 1
fi
test ! -e "$tmp/broken.dts"
printf '%s\n' 'Raw PCM stdin lossless/decoder/truncation: PASS'
