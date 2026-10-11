/* Copyright (C) 2026 the contributors to the libhdma project.
 * SPDX-License-Identifier: LGPL-2.1-or-later.
 * Integer-only XLL acceleration. Future ISA backends implement the same
 * exact scalar contract in separate translation units.
 */
#ifndef LIBHDMA_SIMD_H
#define LIBHDMA_SIMD_H
#include <stddef.h>
#include <stdint.h>
/* Call before creating XLL workers. auto falls back to none on unsupported CPUs.
 * An explicit ISA request fails if it cannot execute on the host. */
int hdma_simd_configure(const char *request);
const char *hdma_simd_selected(void);
int hdma_simd_v3_available(void);
/* All arithmetic on differences wraps modulo 2^32. */
void hdma_simd_differences(int32_t data[512], unsigned order);
/* Return OR of the zigzag codes; bit_length(OR) equals maximum code width. */
uint32_t hdma_simd_zigzag(const int32_t *source, uint32_t *codes, int count);
/* Sum (zigzag >> k) exactly in uint64_t, with no floating-point changes. */
uint64_t hdma_simd_rice_quotients(const uint32_t *codes, int count, unsigned k);
#endif
