/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <arraysumfunctor.hxx>

#if SC_USE_LSX
#include <lsxintrin.h>
#include <cstring>

namespace sc::op
{
namespace
{
// Preserve the Neumaier correction; reassociation/fast-math is not valid here.
void sumLSX(__m128d& sum, __m128d& err, __m128d value)
{
    // vldi takes an encoded immediate; -1 is not a valid all-ones encoding.
    const __m128i absMask = __lsx_vsrli_d(__lsx_vreplgr2vr_d(-1), 1);
    const __m128d total = __lsx_vfadd_d(sum, value);
    const __m128d absSum = (__m128d)__lsx_vand_v((__m128i)sum, absMask);
    const __m128d absValue = (__m128d)__lsx_vand_v((__m128i)value, absMask);
    const __m128i mask = __lsx_vfcmp_cle_d(absValue, absSum);
    // vbitsel selects bits from the second operand where the mask is set.
    const __m128d a = (__m128d)__lsx_vbitsel_v((__m128i)value, (__m128i)sum, mask);
    const __m128d b = (__m128d)__lsx_vbitsel_v((__m128i)sum, (__m128i)value, mask);
    err = __lsx_vfadd_d(err, __lsx_vfadd_d(__lsx_vfsub_d(a, total), b));
    sum = total;
}

__m128d loadPair(const double* values)
{
    // No vector alignment or aliasing requirement; never read past two doubles.
    __m128d value;
    std::memcpy(&value, values, sizeof(value));
    return value;
}
}

KahanSum executeLSX(size_t& i, size_t nSize, const double* pCurrent)
{
    if (i >= nSize || nSize - i < 8)
        return { 0.0, 0.0 };

    const size_t start = i;
    const double* const original = pCurrent;
    __m128d sum0 = (__m128d)__lsx_vldi(0);
    __m128d sum1 = sum0, sum2 = sum0, sum3 = sum0;
    __m128d err0 = sum0, err1 = sum0, err2 = sum0, err3 = sum0;

    for (; nSize - i >= 8; i += 8, pCurrent += 8)
    {
        sumLSX(sum0, err0, loadPair(pCurrent));
        sumLSX(sum1, err1, loadPair(pCurrent + 2));
        sumLSX(sum2, err2, loadPair(pCurrent + 4));
        sumLSX(sum3, err3, loadPair(pCurrent + 6));
    }
    sumLSX(sum0, err0, sum1);
    sumLSX(sum0, err0, err1);
    sumLSX(sum2, err2, sum3);
    sumLSX(sum2, err2, err3);
    sumLSX(sum0, err0, sum2);
    sumLSX(sum0, err0, err2);

    double sums[2], errors[2];
    std::memcpy(sums, &sum0, sizeof(sum0));
    std::memcpy(errors, &err0, sizeof(err0));
    KahanSum::sumNeumaierNormal(sums[0], errors[0], sums[1]);
    KahanSum::sumNeumaierNormal(sums[0], errors[0], errors[1]);
    if (!std::isfinite(sums[0]) || !std::isfinite(errors[0]))
    {
        // Vector FP operations need not preserve Calc's encoded NaN payloads.
        // Replay the original input through the existing scalar path, so that
        // sumArray can still distinguish empty cells from formula errors.
        size_t consumed = 0;
        KahanSum result = executeUnrolled(consumed, nSize - start, original);
        i = start + consumed;
        return result;
    }
    return { sums[0], errors[0] };
}
}
#endif

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
