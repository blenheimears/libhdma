#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Verify an unmistakable core-only melody without changing either lossless
# decoded PCM or, in the normal full-PCM XLL profile, XLL packet bytes.
set -eu
enc=$1; ffmpeg=$2; ffprobe=$3
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
# Enough length to include multiple distinct notes and a deliberate rest.
dd if=/dev/zero of="$scratch/zero.pcm" bs=9216 count=100 2>/dev/null
base="$scratch/zero.pcm --raw-pcm --sample-rate 48000 --channels 6 --bit-depth 24 --channel-layout 5.1 --xll-reference full --no-progress --threads 2"
# shellcheck disable=SC2086
"$enc" $base -o "$scratch/original.dts" >/dev/null
# shellcheck disable=SC2086
"$enc" $base --debug-core-melody -o "$scratch/debug.dts" >"$scratch/log" 2>&1
grep -Fq 'DEBUG CORE MELODY' "$scratch/log"
profile=$("$ffprobe" -v error -f dts -show_entries stream=profile -of default=nw=1:nk=1 "$scratch/debug.dts")
test "$profile" = 'DTS-HD MA'
"$ffmpeg" -v error -xerror -f dts -i "$scratch/debug.dts" -c:a pcm_s24le -f s24le -y "$scratch/decoded.pcm"
head -c "$(wc -c < "$scratch/zero.pcm")" "$scratch/decoded.pcm" > "$scratch/trimmed.pcm"
cmp "$scratch/zero.pcm" "$scratch/trimmed.pcm"
"$ffmpeg" -v error -xerror -core_only 1 -f dts -i "$scratch/debug.dts" -c:a pcm_s24le -f s24le -y "$scratch/core.pcm"
# Core-only output must not be the all-zero source, and normal core and test
# core must not be identical. Both are checked for decodability.
test -s "$scratch/core.pcm"
if cmp -s "$scratch/core.pcm" "$scratch/zero.pcm"; then
    echo 'diagnostic core unexpectedly equals original silence' >&2; exit 1
fi
"$ffmpeg" -v error -xerror -core_only 1 -f dts -i "$scratch/original.dts" -c:a pcm_s24le -f s24le -y "$scratch/original-core.pcm"
if cmp -s "$scratch/original-core.pcm" "$scratch/core.pcm"; then
    echo 'diagnostic core unchanged' >&2; exit 1
fi
python3 - "$scratch/original.dts" "$scratch/debug.dts" <<'PY'
import sys
sync=b'\x7f\xfe\x80\x01'; exss=b'\x64\x58\x20\x25'
a,b=(open(path,'rb').read() for path in sys.argv[1:])
assert a!=b and len(a)==len(b), 'debug should change only the core frame payload'
pos=0; count=0
while pos < len(a):
    assert a[pos:pos+4]==sync and b[pos:pos+4]==sync, 'missing DCA core sync'
    assert a[pos+2012:pos+2016]==exss and b[pos+2012:pos+2016]==exss, 'wrong EXSS offset'
    next_a=a.find(sync,pos+2016)
    next_b=b.find(sync,pos+2016)
    assert next_a==next_b, 'different frame boundaries'
    end=len(a) if next_a<0 else next_a
    assert a[pos+2012:end]==b[pos+2012:end], 'lossless extension bytes changed'
    pos=end; count+=1
assert count>=2, 'not enough frames'
print('PASS independent full-PCM XLL bytes identical:',count,'frames')
PY
# Fail safely: a core-only encode or observed core-dependent profile must not
# be allowed to imply that the original lossless track was preserved.
for bad in '--lossy' '--xll-reference core'; do
    # shellcheck disable=SC2086
    if "$enc" $base --debug-core-melody $bad -o "$scratch/bad.dts" >/dev/null 2>&1; then
        echo "incompatible diagnostic flags accepted: $bad" >&2; exit 1
    fi
    test ! -e "$scratch/bad.dts"
done
printf '%s\n' 'PASS debug melody: DCA + XLL, distinct decodable core, exact original lossless PCM, invalid modes rejected'

# Failed experimental encoders must not silently remain available.
for retired in --observed-5.1; do
    if "$enc" "$scratch/zero.pcm" --raw-pcm --sample-rate 48000 --channels 6 --bit-depth 24 \
        --channel-layout 5.1 "$retired" -o "$scratch/retired.dts" > /dev/null 2>&1; then
        echo "retired experiment still accepted: $retired" >&2; exit 1
    fi
    test ! -e "$scratch/retired.dts"
done
printf '%s\n' 'PASS retired encoding profiles cannot be invoked'
