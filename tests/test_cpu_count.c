/* Runtime CPU-count regression.
 * Copyright (C) 2026 the contributors to the libhdma project. SPDX-License-Identifier: LGPL-2.1-or-later.
 */
#include "cpu_count.h"
#include <stdio.h>

int main(void)
{
    unsigned threads = hdma_default_thread_count();
    if (threads < 1 || threads > 16) {
        fprintf(stderr, "invalid default thread count: %u\n", threads);
        return 1;
    }
    printf("default threads: %u\n", threads);
    return 0;
}
