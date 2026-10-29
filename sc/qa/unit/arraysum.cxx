/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <sal/config.h>
#include <arraysumfunctor.hxx>
// Compile the production implementations into a test-owned translation unit.
// Reusing their gbuild objects would link them into both sc and this test
// (fdo#47246); linking sc instead would require exporting internal functions.
#include "../../source/core/tool/arraysumSSE2.cxx"
#include "../../source/core/tool/arraysumLSX.cxx"

#if defined(SC_ARRAYSUM_REQUIRE_LSX) && !SC_USE_LSX
#error The explicit LSX test must compile with LSX enabled
#endif
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace
{
class ArraySumTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(ArraySumTest);
    CPPUNIT_TEST(testLengthsAndAlignment);
    CPPUNIT_TEST(testCompensation);
    CPPUNIT_TEST(testExactAllocation);
    CPPUNIT_TEST(testErrors);
    CPPUNIT_TEST(testLSXOffset);
    CPPUNIT_TEST_SUITE_END();

public:
    void testLengthsAndAlignment()
    {
        // Every possible tail, including addresses 8/16/24 bytes past alignment.
        alignas(32) std::array<double, 264> values;
        for (size_t offset = 0; offset < 4; ++offset)
        {
            for (size_t length = 0; length <= 257; ++length)
            {
                values.fill(std::numeric_limits<double>::quiet_NaN());
                double expected = 0;
                for (size_t j = 0; j < length; ++j)
                {
                    values[offset + j] = static_cast<int>(j % 17) - 8;
                    expected += values[offset + j];
                }
                CPPUNIT_ASSERT_EQUAL(expected, sc::op::sumArray(values.data() + offset, length).get());
            }
        }
        CPPUNIT_ASSERT_EQUAL(0.0, sc::op::sumArray(nullptr, 0).get());
    }

    void testCompensation()
    {
        // Large/small terms occur in the same SIMD lane, so naive summation
        // loses the unit contributions. Includes all scalar-tail lengths.
        for (size_t tail = 0; tail < 8; ++tail)
        {
            std::array<double, 199> values{};
            for (size_t block = 0; block < 8; ++block)
            {
                std::fill_n(values.data() + block * 24, 8, 1e16);
                std::fill_n(values.data() + block * 24 + 8, 8, 1.0);
                std::fill_n(values.data() + block * 24 + 16, 8, -1e16);
            }
            std::fill_n(values.data() + 192, tail, 0.25);
            CPPUNIT_ASSERT_EQUAL(64.0 + tail * 0.25,
                                 sc::op::sumArray(values.data(), 192 + tail).get());
        }
    }

    void testExactAllocation()
    {
        // Under ASan, a full-vector load beyond the tail crosses the redzone.
        for (size_t length = 1; length < 80; ++length)
        {
            auto values = std::make_unique<double[]>(length);
            std::fill_n(values.get(), length, 0.5);
            CPPUNIT_ASSERT_EQUAL(length * 0.5, sc::op::sumArray(values.get(), length).get());
        }
    }

    void testErrors()
    {
        for (size_t pos = 0; pos < 33; ++pos)
        {
            std::array<double, 33> values;
            values.fill(1.0);
#ifndef DISABLE_NAN_TESTS
            // Encoded errors require preservation of NaN payloads.
            values[pos] = CreateDoubleError(FormulaError::NoValue);
            CPPUNIT_ASSERT_EQUAL(32.0, sc::op::sumArray(values.data(), values.size()).get());
            values[pos] = CreateDoubleError(FormulaError::DivisionByZero);
            CPPUNIT_ASSERT_EQUAL(FormulaError::DivisionByZero,
                                 GetDoubleErrorValue(sc::op::sumArray(values.data(), values.size()).get()));
#endif
            values[pos] = std::numeric_limits<double>::infinity();
            CPPUNIT_ASSERT(!std::isfinite(sc::op::sumArray(values.data(), values.size()).get()));
        }
    }

    void testLSXOffset()
    {
#if SC_USE_LSX
        std::array<double, 24> values;
        values.fill(1.0);
        size_t i = 3;
        KahanSum sum = sc::op::executeLSX(i, 22, values.data() + 3);
        CPPUNIT_ASSERT_EQUAL(size_t(19), i);
        for (; i < 22; ++i)
            sum += values[i];
        CPPUNIT_ASSERT_EQUAL(19.0, sum.get());
#ifndef DISABLE_NAN_TESTS
        values[5] = CreateDoubleError(FormulaError::NoValue);
        i = 3;
        sum = sc::op::executeLSX(i, 22, values.data() + 3);
        CPPUNIT_ASSERT_EQUAL(size_t(19), i);
        CPPUNIT_ASSERT_EQUAL(FormulaError::NoValue, GetDoubleErrorValue(sum.get()));
#endif
#endif
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(ArraySumTest);
}
CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
