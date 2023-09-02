/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 */

// This is a C file - to avoid inclusion of C++ headers
// since compiling with different instruction set can generate
// versions of inlined code that get injected outside of this
// module by the linker.

#include <config.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "DeltaSimd.h"

#if ENABLE_SIMD
#  include <immintrin.h>

#define DEBUG_LUT 0

// set of control data bytes for vperd
static __m256i vpermd_lut[256];

// Build table we can lookup bitmasks in to generate gather data
void init_gather_lut()
{
    for (unsigned int pattern = 0; pattern < 256; ++pattern)
    {
        unsigned int i = 0, src = 0;
        uint8_t lut[8];
        for (uint32_t bitToCheck = 1; bitToCheck < 256; bitToCheck <<= 1)
        {
            if (!(pattern & bitToCheck)) // set bit is a duplicate -> ignore.
                lut[i++] = src;
            src++;
        }
        while (i<8) // pad to copy first point
                lut[i++] = 0;

#if DEBUG_LUG
        fprintf(stderr, "lut mask: 0x%x generates %d %d %d %d %d %d %d %d\n",
                pattern, lut[7], lut[6], lut[5], lut[4], lut[3], lut[2], lut[1], lut[0]);
#endif
        vpermd_lut[pattern] = _mm256_set_epi8(
            0, 0, 0, lut[7],  0, 0, 0, lut [6],
            0, 0, 0, lut[5],  0, 0, 0, lut [4],
            0, 0, 0, lut[3],  0, 0, 0, lut [2],
            0, 0, 0, lut[1],  0, 0, 0, lut [0]);
    }
}

// non-intuitively we need to use the sign bit as
// if floats to gather bits from 32bit words
static uint64_t diffMask(__m256i prev, __m256i curr)
{
    __m256i res = _mm256_cmpeq_epi32(prev, curr);
    __m256 m256 = _mm256_castsi256_ps(res);
    return _mm256_movemask_ps(m256);
}

#endif

// accelerated compression of a 256 pixel run
int simd_initPixRowSimd(const uint32_t *from, uint32_t *scratch, unsigned int *scratchLen, uint64_t *rleMaskBlock)
{
#if !ENABLE_SIMD
    // no fun.
    (void)from; (void)scratch; (void)scratchLen; (void)rleMask;
    return 0;

#else // ENABLE_SIMD

    static int lut_initialized = 0;
    if (!lut_initialized)
    {
        lut_initialized = 1;
        init_gather_lut();
    }

    *scratchLen = 0;

    unsigned int x = 0;
    const uint32_t* block = from;
    for (unsigned int nMask = 0; nMask < 4; ++nMask)
    {
        uint64_t rleMask = 0;
        uint64_t newMask = 0;
        int remaining = 256 - x;
        assert(remaining % 8 == 0);
        int blocks = remaining/8;
        if (blocks > 8)
            blocks = 8;
        for (int i = 0; i < blocks; ++i)
        {
            __m256i prev;
            __m256i curr = _mm256_loadu_si256((const __m256i_u*)(block));

            // Generate mask
            switch (x)
            {
            case 0:
            {
                prev = _mm256_set_epi32(block[6], block[5], block[4], block[3],
                                        block[2], block[1], block[0], 0 /* transparent */);
                break;
            }
            default:
            {
                prev = _mm256_loadu_si256((const __m256i_u*)(block - 1));
                break;
            }
            }
            newMask = diffMask(prev, curr);
            rleMask |= newMask << (i * 8);

            assert (newMask < 256);
            __m256i control_vector = _mm256_loadu_si256(&vpermd_lut[newMask]);
            __m256i packed = _mm256_permutevar8x32_epi32(curr, control_vector);

            unsigned int countBitsUnset = _mm_popcnt_u32(newMask ^ 0xff);
            assert(countBitsUnset <= 8);

            // we are guaranteed enough space worst-case
            _mm256_storeu_si256((__m256i*)scratch, packed);

#if DEBUG_LUT
            if (countBitsUnset > 0)
                fprintf(stderr, "for mask: 0x%2x bits-unset %d we have:\n"
                        "%4x%4x%4x%4x%4x%4x%4x%4x\n"
                        "%4x%4x%4x%4x%4x%4x%4x%4x\n",
                        (unsigned int)newMask, countBitsUnset,
                        block[0], block[1], block[2], block[3], block[4], block[5], block[6], block[7],
                        scratch[0], scratch[1], scratch[2], scratch[3], scratch[4], scratch[5], scratch[6], scratch[7]);
#endif

            scratch += countBitsUnset;
            *scratchLen += countBitsUnset;

            block += 8;
            x += 8;
        }
        rleMaskBlock[nMask] = rleMask;
    }

    return 1;
#endif
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
