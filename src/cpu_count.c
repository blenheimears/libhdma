/* Runtime CPU-count helper for hdmaenc.
 * Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* Apple SDK sysctl headers still use BSD compatibility typedefs such as
 * u_int/u_char.  _POSIX_C_SOURCE hides those names unless Darwin extensions
 * are explicitly requested.  Keep the POSIX feature level while restoring
 * the SDK declarations required by <sys/sysctl.h>. */
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "cpu_count.h"

#include <stddef.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

static unsigned clamp_thread_count(long cpus)
{
    if (cpus < 1)
        return 1;
    return cpus > 16 ? 16U : (unsigned)cpus;
}

unsigned hdma_default_thread_count(void)
{
#if defined(__APPLE__)
    /* _SC_NPROCESSORS_ONLN is not exposed by all macOS SDK configurations.
     * hw.logicalcpu reports the logical CPUs currently available to the OS.
     * Fall back to hw.ncpu for older Darwin releases/SDK combinations. */
    int cpus = 0;
    size_t size = sizeof(cpus);
    if (sysctlbyname("hw.logicalcpu", &cpus, &size, NULL, 0) == 0 &&
        size == sizeof(cpus) && cpus > 0)
        return clamp_thread_count(cpus);

    cpus = 0;
    size = sizeof(cpus);
    if (sysctlbyname("hw.ncpu", &cpus, &size, NULL, 0) == 0 &&
        size == sizeof(cpus) && cpus > 0)
        return clamp_thread_count(cpus);
#endif

#ifdef _SC_NPROCESSORS_ONLN
    return clamp_thread_count(sysconf(_SC_NPROCESSORS_ONLN));
#else
    return 1;
#endif
}
