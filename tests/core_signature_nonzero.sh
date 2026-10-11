# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
# Independent non-silent PCM, core and XLL acceptance across 2.1, 5.1, 7.1.
set -eu
encoder=$1
ffmpeg=$2
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
python3 - "$scratch" <<'PY'
import pathlib,struct,sys
p=pathlib.Path(sys.argv[1]); n=9216
for channels,layout in [(6,'51'),(8,'71'),(3,'21')]:
    with (p / f'{layout}.pcm').open('wb') as f:
        for i in range(n):
            for c in range(channels):
                sample=((((i*1171+c*34871+i*i*5)%13000001)-6500000)//(1 if c else 2))
                f.write(int(sample).to_bytes(3,'little',signed=True))
PY
for layout in 51 71 21; do
  case "$layout" in 51) channels=6; cl=5.1;; 71) channels=8; cl=7.1;; 21) channels=3; cl=2.1;; esac
  "$encoder" "$scratch/$layout.pcm" --raw-pcm --sample-rate 48000 --channels "$channels" --bit-depth 24 --channel-layout "$cl" --xll-reference full --no-progress --threads 2 -o "$scratch/$layout-normal.dts" >/dev/null
  "$encoder" "$scratch/$layout.pcm" --raw-pcm --sample-rate 48000 --channels "$channels" --bit-depth 24 --channel-layout "$cl" --xll-reference full --debug-core-melody --no-progress --threads 2 -o "$scratch/$layout-debug.dts" >/dev/null 2>"$scratch/warnings"
  grep -Fq 'WARNING: --debug-core-melody' "$scratch/warnings"
  "$ffmpeg" -v error -xerror -f dts -i "$scratch/$layout-debug.dts" -c:a pcm_s24le -f s24le -y "$scratch/$layout-output.pcm"
  head -c "$(wc -c < "$scratch/$layout.pcm")" "$scratch/$layout-output.pcm" > "$scratch/$layout-trim.pcm"
  cmp "$scratch/$layout.pcm" "$scratch/$layout-trim.pcm"
  "$ffmpeg" -v error -xerror -core_only 1 -f dts -i "$scratch/$layout-debug.dts" -c:a pcm_s24le -f s24le -y "$scratch/$layout-core.pcm"
  python3 - "$scratch/$layout-normal.dts" "$scratch/$layout-debug.dts" <<'PY'
import sys
sync=b'\x7f\xfe\x80\x01'; exss=b'\x64\x58\x20\x25'
a,b=[open(p,'rb').read() for p in sys.argv[1:]]
assert len(a)==len(b) and a!=b
p=0;k=0
while p<len(a):
    assert a[p:p+4]==sync and b[p:p+4]==sync
    assert a[p+2012:p+2016]==exss and b[p+2012:p+2016]==exss
    end=a.find(sync,p+2016)
    assert end==b.find(sync,p+2016)
    if end<0:end=len(a)
    assert a[p+2012:end]==b[p+2012:end],f'XLL changed at {k}'
    p=end;k+=1
assert k>=4
print('PASS lossless extension bytes unchanged:',k,'packets')
PY
  printf 'PASS nonzero input %s original PCM bit exact, core independently decodable\n' "$cl"
done
