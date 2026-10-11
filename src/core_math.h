/* Fixed-point math adapted from FFmpeg 9.0.2 dcamath.h, LGPL-2.1-or-later.
 * Copyright (C) 2016 foo86. Original license in COPYING.LGPL-2.1.
 * Standalone adaptations: Copyright (C) 2026 the contributors to the libhdma project.
 */
#ifndef DCA_CORE_MATH_H
#define DCA_CORE_MATH_H
#include <stdint.h>
#include <limits.h>
static inline int32_t norm__(int64_t a, int bits) {
    return bits > 0 ? (int32_t)((a + (INT64_C(1) << (bits - 1))) >> bits) : (int32_t)a;
}
static inline int32_t mul32(int32_t a, int32_t b) {
    return norm__((int64_t)a * b, 32);
}
#endif
