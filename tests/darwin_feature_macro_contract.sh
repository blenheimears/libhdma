#!/bin/sh
# Copyright (C) 2026 the contributors to the libhdma project
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
source_dir=$1
f="$source_dir/src/cpu_count.c"
grep -q '^#define _DARWIN_C_SOURCE 1$' "$f"
grep -q '^#define _POSIX_C_SOURCE 200809L$' "$f"
grep -q '^#include <sys/sysctl.h>$' "$f"
darwin_line=$(grep -n '^#define _DARWIN_C_SOURCE 1$' "$f" | cut -d: -f1)
posix_line=$(grep -n '^#define _POSIX_C_SOURCE 200809L$' "$f" | cut -d: -f1)
sysctl_line=$(grep -n '^#include <sys/sysctl.h>$' "$f" | cut -d: -f1)
[ "$darwin_line" -lt "$posix_line" ]
[ "$posix_line" -lt "$sysctl_line" ]
printf '%s\n' 'Darwin feature-macro ordering: PASS'
