/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <vcl/BinaryDataContainer.hxx>

using namespace css;

namespace
{
class BinaryDataContainerTest : public CppUnit::TestFixture
{
    void testConstruct();
    void testPinnedStorage();

    CPPUNIT_TEST_SUITE(BinaryDataContainerTest);
    CPPUNIT_TEST(testConstruct);
    CPPUNIT_TEST(testPinnedStorage);
    CPPUNIT_TEST_SUITE_END();
};

void BinaryDataContainerTest::testConstruct()
{
    {
        BinaryDataContainer aContainer;
        CPPUNIT_ASSERT(aContainer.isEmpty());
        CPPUNIT_ASSERT_EQUAL(size_t(0), aContainer.getSize());
    }
    {
        // construct a data array
        sal_uInt8 aTestByteArray[] = { 1, 2, 3, 4 };
        SvMemoryStream stream(aTestByteArray, std::size(aTestByteArray), StreamMode::READ);

        BinaryDataContainer aContainer(stream, std::size(aTestByteArray));

        CPPUNIT_ASSERT(!aContainer.isEmpty());
        CPPUNIT_ASSERT_EQUAL(size_t(4), aContainer.getSize());

        // Test Copy
        BinaryDataContainer aCopyOfContainer = aContainer;
        CPPUNIT_ASSERT(!aCopyOfContainer.isEmpty());
        CPPUNIT_ASSERT_EQUAL(size_t(4), aCopyOfContainer.getSize());
        CPPUNIT_ASSERT_EQUAL(aCopyOfContainer.getData(), aContainer.getData());

        // Test Move
        BinaryDataContainer aMovedInContainer = std::move(aCopyOfContainer);
        CPPUNIT_ASSERT(!aMovedInContainer.isEmpty());
        CPPUNIT_ASSERT_EQUAL(size_t(4), aMovedInContainer.getSize());
        CPPUNIT_ASSERT_EQUAL(aMovedInContainer.getData(), aContainer.getData());

        CPPUNIT_ASSERT(aCopyOfContainer.isEmpty());
        CPPUNIT_ASSERT_EQUAL(size_t(0), aCopyOfContainer.getSize());
    }
}

void BinaryDataContainerTest::testPinnedStorage()
{
    CPPUNIT_ASSERT(!BinaryDataContainer().getDataShared());
    std::shared_ptr<const std::vector<sal_uInt8>> pinned;
    {
        sal_uInt8 data[] = { 1, 2, 3, 4 };
        SvMemoryStream stream(data, std::size(data), StreamMode::READ);
        BinaryDataContainer container(stream, std::size(data));
        pinned = container.getDataShared();
        CPPUNIT_ASSERT(pinned);
        // The worker gets an owning, immutable view without a full-file copy.
        CPPUNIT_ASSERT_EQUAL(container.getData(), pinned->data());
    }
    // PDFium may retain this pointer after the original container is gone.
    CPPUNIT_ASSERT_EQUAL(size_t(4), pinned->size());
    CPPUNIT_ASSERT_EQUAL(sal_uInt8(1), pinned->front());
    CPPUNIT_ASSERT_EQUAL(sal_uInt8(4), pinned->back());
}

} // namespace

CPPUNIT_TEST_SUITE_REGISTRATION(BinaryDataContainerTest);

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
