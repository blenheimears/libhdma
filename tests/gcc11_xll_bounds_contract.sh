#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
root=${1:?source root required}
xll="$root/src/xll.c"
# GCC 11's object-size pass must see the output as a complete fixed-size row,
# not as an int32_t * produced by array-parameter decay. Keep the public
# algorithm/storage unchanged while carrying the row type through the helper.
grep -Fq 'typedef int32_t XllSampleRow[HDMA_XLL_SAMPLES];' "$xll"
grep -Fq 'XllSampleRow *transformed)' "$xll"
grep -Fq 'XllSampleRow transformed[8];' "$xll"
grep -Fq '&initial[c],&transformed[c]);' "$xll"
grep -Fq 'memcpy(*transformed,candidate_data,sizeof(candidate_data));' "$xll"
# Keep the local channel invariant from 0.0.41 as an independent safety check.
grep -Fq 'if(ch < 1 || ch > 8) return -1;' "$xll"
grep -Fq 'for(int c=0;c<ch && c<8;c++) {' "$xll"
# Do not paper over the portable-build diagnostic with a warning suppression.
if grep -R -- '-Wno-stringop-overflow' "$root/CMakeLists.txt" "$root/src" >/dev/null 2>&1; then
    echo 'error: stringop-overflow warning is being suppressed instead of fixing the row type' >&2
    exit 1
fi
