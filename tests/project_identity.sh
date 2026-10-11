#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
source_dir=$1
encoder=$2
[ -x "$encoder" ]
case "$("$encoder" --version)" in
    'hdmaenc 0.0.49-'* ) ;;
    *) echo 'Unexpected frontend identity' >&2; exit 1 ;;
esac
[ -f "$source_dir/include/libhdma.h" ]
[ -f "$source_dir/flake.nix" ]
grep -q 'pname = "libhdma";' "$source_dir/flake.nix"
grep -q 'mainProgram = "hdmaenc";' "$source_dir/flake.nix"
grep -q 'wrapProgram "$out/bin/hdmaenc"' "$source_dir/flake.nix"
grep -q 'project(libhdma VERSION 0.0.49' "$source_dir/CMakeLists.txt"
for file in "$source_dir"/include/*.h "$source_dir"/src/*.c "$source_dir"/src/*.h "$source_dir"/tests/*.c "$source_dir"/tests/*.sh; do
    grep -q 'Copyright (C) 2026 the contributors to the libhdma project' "$file" || {
        echo "Missing contributor notice: $file" >&2; exit 1;
    }
done
for file in core.c core_params.h core_tables.c core_math.h core_adpcm.c core_adpcm_tables.c; do
    grep -q 'Copyright (C)' "$source_dir/src/$file" || exit 1
done
grep -q 'Alexander E. Patrakov' "$source_dir/src/core.c"
grep -q 'Benjamin Larsson' "$source_dir/src/core.c"
grep -q 'Xiang Wang' "$source_dir/src/core.c"
grep -q 'Gildas Bazin' "$source_dir/src/core_tables.c"
grep -q 'Copyright (C) 2016 foo86' "$source_dir/src/core_math.h"
printf '%s\n' 'Project identity, source notices and upstream credits: PASS'

# Only the minimal ADPCM predictor and codebook are included; no FFmpeg tree.
[ -f "$source_dir/src/core_adpcm.c" ] && [ -f "$source_dir/src/core_adpcm_tables.c" ]
[ ! -d "$source_dir/libavcodec" ]
