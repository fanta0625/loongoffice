/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include <sal/types.h>
#include "layout.hxx"
#include "cache.hxx"
#include "demand.hxx"
#include "transfer.hxx"
#include "viewpolicy.hxx"
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>
#include <algorithm>
#include <limits>
#include <map>
#include <vector>
#include <random>

namespace
{
class CoreTest final : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(CoreTest);
    CPPUNIT_TEST(testLayoutAgainstLinearReference);
    CPPUNIT_TEST(testPageIdentityAndAnchor);
    CPPUNIT_TEST(testShortLastPageNavigation);
    CPPUNIT_TEST(testInvalidGeometry);
    CPPUNIT_TEST(testDemandReuseAndPriority);
    CPPUNIT_TEST(testRegionBatchAndPreemption);
    CPPUNIT_TEST(testRegionBoundaries);
    CPPUNIT_TEST(testTransferBudget);
    CPPUNIT_TEST(testCacheSelectiveInvalidation);
    CPPUNIT_TEST(testVisibleCacheProtection);
    CPPUNIT_TEST(testPreviewSurvivesOversizedWorkingSet);
    CPPUNIT_TEST(testFallbackCoverage);
    CPPUNIT_TEST(testCacheEviction);
    CPPUNIT_TEST(testReplacementAndOversizedEntry);
    CPPUNIT_TEST(testZoomSettle);
    CPPUNIT_TEST(testFallbackLevel);
    CPPUNIT_TEST_SUITE_END();

public:
    void testZoomSettle()
    {
        using namespace std::chrono;
        const steady_clock::time_point start{};
        pdffast::ZoomSettle zoom;
        zoom.update(0, 600, 800, start);
        CPPUNIT_ASSERT(zoom.pending(start));
        CPPUNIT_ASSERT(!zoom.pending(start + milliseconds(120)));
        zoom.update(0, 900, 1200, start + milliseconds(10));
        CPPUNIT_ASSERT(zoom.pending(start + milliseconds(89)));
        // A preview delivery/scroll at the same scale must not extend settling.
        zoom.update(0, 900, 1200, start + milliseconds(60));
        CPPUNIT_ASSERT(!zoom.pending(start + milliseconds(90)));
        zoom.update(0, 1200, 1600, start + milliseconds(100));
        zoom.update(0, 1500, 2000, start + milliseconds(150));
        CPPUNIT_ASSERT(zoom.pending(start + milliseconds(229)));
        CPPUNIT_ASSERT(!zoom.pending(start + milliseconds(230)));
        zoom.update(1, 600, 800, start + milliseconds(160));
        CPPUNIT_ASSERT(zoom.pending(start + milliseconds(160)));
        CPPUNIT_ASSERT(!zoom.pending(start + milliseconds(280)));
    }
    void testFallbackLevel()
    {
        using pdffast::TileKey;
        const std::vector<TileKey> levels{
            { 0, 300, 400, 0, 0, 300, 400 },
            { 0, 1200, 1600, 0, 0, 512, 512 },
            { 0, 1800, 2400, 0, 0, 512, 512 },
            { 0, 0, 0, 0, 0, 1, 1 }
        };
        CPPUNIT_ASSERT((pdffast::fallbackLevel(levels, 1000, 1333)
                        == std::pair<int, int>(1200, 1600)));
        CPPUNIT_ASSERT((pdffast::fallbackLevel(levels, 1700, 2267)
                        == std::pair<int, int>(1800, 2400)));
        CPPUNIT_ASSERT((pdffast::fallbackLevel({}, 1000, 1000) == std::pair<int, int>()));
        CPPUNIT_ASSERT((pdffast::fallbackLevel(levels, 0, 1000) == std::pair<int, int>()));
    }

    void testLayoutAgainstLinearReference()
    {
        // Out-of-order sizes, repeated changes and page lookup against an
        // independent linear layout. Includes gaps and exact page boundaries.
        pdffast::Layout layout;
        layout.reset(10000, { 600, 800 });
        std::vector<double> heights(10000, 800);
        std::mt19937 random(42);
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            const int page = random() % heights.size();
            const double height = 100 + random() % 2000;
            layout.set(page, { 600, height });
            heights[page] = height;
            double top = pdffast::Layout::Gap;
            for (int index = 0; index < layout.count(); ++index)
            {
                CPPUNIT_ASSERT_DOUBLES_EQUAL(top, layout.top(index), 1e-8);
                if (index % 137 == 0)
                {
                    CPPUNIT_ASSERT_EQUAL(index, layout.pageAt(top));
                    CPPUNIT_ASSERT_EQUAL(index, layout.pageAt(top + heights[index] / 2));
                    CPPUNIT_ASSERT_EQUAL(index, layout.pageAt(top + heights[index] + 1));
                }
                top += heights[index] + pdffast::Layout::Gap;
            }
            CPPUNIT_ASSERT_DOUBLES_EQUAL(top, layout.height(), 1e-8);
        }
        CPPUNIT_ASSERT_EQUAL(0, layout.pageAt(-100));
        CPPUNIT_ASSERT_EQUAL(9999, layout.pageAt(layout.height() + 100));
    }
    void testPageIdentityAndAnchor()
    {
        pdffast::Layout layout;
        layout.reset(100, { 600, 800 });
        const int anchor = 70;
        const double offset = 123;
        layout.set(3, { 1200, 1600 });
        layout.set(50, { 300, 400 });
        const double scroll = layout.top(anchor) + offset;
        CPPUNIT_ASSERT_EQUAL(anchor, layout.pageAt(scroll));
        CPPUNIT_ASSERT(!layout.known(anchor));
        layout.set(anchor, { 500, 700 });
        CPPUNIT_ASSERT_EQUAL(anchor, layout.pageAt(scroll));
        CPPUNIT_ASSERT(layout.known(anchor));
        CPPUNIT_ASSERT_DOUBLES_EQUAL(123, scroll - layout.top(anchor), 1e-8);
        CPPUNIT_ASSERT_DOUBLES_EQUAL(1224, layout.width(), 1e-8);
    }
    void testShortLastPageNavigation()
    {
        pdffast::Layout layout;
        layout.reset(2, { 842, 595 }); // Landscape A4, both shorter than the viewport.
        const double viewportPixels = 834;
        for (double scale : { 0.5, 96.0 / 72.0, 2.0 })
        {
            const double maximum = layout.scrollHeight(viewportPixels / scale) * scale
                                   - viewportPixels;
            const double destination = std::clamp(layout.top(1) * scale, 0.0, maximum);
            CPPUNIT_ASSERT_EQUAL(1, layout.pageAt(destination / scale));
        }
        // Metadata may reveal that the final page is even shorter.
        layout.set(1, { 842, 100 });
        const double viewport = 1000;
        const double destination = std::min(layout.top(1), layout.scrollHeight(viewport) - viewport);
        CPPUNIT_ASSERT_EQUAL(1, layout.pageAt(destination));
        CPPUNIT_ASSERT_DOUBLES_EQUAL(layout.top(1), destination, 1e-8);
        // A one-page document must also retain a nonnegative scroll range.
        layout.reset(1, { 842, 100 });
        CPPUNIT_ASSERT(layout.scrollHeight(viewport) >= viewport);
    }
    void testInvalidGeometry()
    {
        pdffast::Layout layout;
        CPPUNIT_ASSERT_THROW(layout.reset(0, { 600, 800 }), std::invalid_argument);
        CPPUNIT_ASSERT_THROW(layout.reset(2, { 0, 800 }), std::invalid_argument);
        CPPUNIT_ASSERT_THROW(layout.reset(1000001, { 600, 800 }), std::invalid_argument);
        layout.reset(2, { 600, 800 });
        const double nan = std::numeric_limits<double>::quiet_NaN();
        CPPUNIT_ASSERT_THROW(layout.set(1, { nan, 800 }), std::invalid_argument);
        CPPUNIT_ASSERT_THROW(layout.set(1, { 600, -1 }), std::invalid_argument);
        CPPUNIT_ASSERT_THROW(layout.set(2, { 600, 800 }), std::out_of_range);
        CPPUNIT_ASSERT_DOUBLES_EQUAL(1636, layout.height(), 1e-8);
    }
    void testDemandReuseAndPriority()
    {
        const pdffast::Request first{ 0, { 0, 1000, 1400, 0, 0, 512, 512 } };
        const pdffast::Request second{ 0, { 0, 1000, 1400, 0, 512, 512, 512 } };
        const pdffast::Request otherPage{ 1, { 1, 1000, 1400, 0, 0, 512, 512 } };
        const pdffast::Request resized{ 0, { 0, 2000, 2800, 0, 0, 512, 512 } };
        pdffast::Demand demand;
        demand.update({ first, second, otherPage }, {});
        CPPUNIT_ASSERT(demand.next() == first);
        // A small scroll retains the in-flight tile. A completed tile waiting
        // for the UI must not be rerendered; new priority order still wins.
        demand.update({ first, otherPage, second, second }, { second });
        CPPUNIT_ASSERT(demand.needs(first));
        demand.finish();
        CPPUNIT_ASSERT(demand.next() == otherPage);
        demand.finish();
        CPPUNIT_ASSERT(demand.empty());
        demand.update({ first, second }, {});
        CPPUNIT_ASSERT(demand.next() == first);
        demand.update({ resized }, {});
        CPPUNIT_ASSERT(!demand.needs(first)); // Obsolete scale is not published.
        // Returning to a size before its render completes reuses the same task.
        demand.update({ first, second }, {});
        CPPUNIT_ASSERT(demand.needs(first));
        demand.finish();
        CPPUNIT_ASSERT(demand.next() == second);
        demand.finish();
        demand.update({}, {});
        CPPUNIT_ASSERT(demand.empty());
        CPPUNIT_ASSERT(!demand.needs(second));
    }
    void testRegionBatchAndPreemption()
    {
        const pdffast::Request a{ 0, { 0, 4096, 4096, 0, 0, 512, 512 }, true };
        const pdffast::Request b{ 0, { 0, 4096, 4096, 512, 0, 512, 512 }, true };
        const pdffast::Request visible{ 1, { 1, 4096, 4096, 0, 0, 512, 512 } };
        pdffast::Demand demand;
        demand.update({ a, b }, {});
        CPPUNIT_ASSERT_EQUAL(std::size_t(2), demand.nextBatch().size());
        demand.update({ visible, b }, {}); // a left the viewport; b is still in flight.
        CPPUNIT_ASSERT(!demand.needs(a));
        CPPUNIT_ASSERT(demand.needs(b));
        CPPUNIT_ASSERT(demand.hasForeground());
        demand.finish(true); // Retry only b, behind the new visible task.
        CPPUNIT_ASSERT(demand.nextBatch().front() == visible);
        demand.finish();
        CPPUNIT_ASSERT(demand.nextBatch().front() == b);
        demand.update({}, {});
        demand.finish(true);
        CPPUNIT_ASSERT(demand.empty());
        auto promoted = a;
        promoted.prefetch = false;
        demand.update({ promoted, b }, {});
        CPPUNIT_ASSERT_EQUAL(std::size_t(1), demand.nextBatch().size());
        demand.finish(); // Foreground and background must not merge.
        demand.update({ a }, { promoted });
        CPPUNIT_ASSERT(demand.empty()); // Promotion does not change identity.
    }
    void testRegionBoundaries()
    {
        const pdffast::TileKey first{ 0, 4096, 4096, 0, 0, 512, 512 };
        auto area = first;
        for (int x : { 512, 1024, 1536 })
        {
            auto child = first;
            child.x = x;
            auto joined = pdffast::mergeRegion(area, child);
            CPPUNIT_ASSERT(joined.has_value());
            area = *joined;
        }
        CPPUNIT_ASSERT_EQUAL(2048, area.width);
        auto child = first;
        child.x = 2048;
        CPPUNIT_ASSERT(!pdffast::mergeRegion(area, child)); // Area/width bound.
        child.x = 513;
        CPPUNIT_ASSERT(!pdffast::mergeRegion(first, child)); // A gap cannot be hidden.
        child.x = 512;
        child.page = 1;
        CPPUNIT_ASSERT(!pdffast::mergeRegion(first, child));
        child.page = 0;
        child.pageWidth = 8192;
        CPPUNIT_ASSERT(!pdffast::mergeRegion(first, child));
        const pdffast::TileKey preview{ 0, 256, 512, 0, 0, 256, 512 };
        CPPUNIT_ASSERT(!pdffast::mergeRegion(preview, preview));
        const pdffast::TileKey edge{ 0, 700, 900, 512, 0, 188, 512 };
        auto left = first;
        left.pageWidth = 700;
        left.pageHeight = 900;
        const auto joined = pdffast::mergeRegion(left, edge);
        CPPUNIT_ASSERT(joined.has_value());
        CPPUNIT_ASSERT_EQUAL(700, joined->width);
    }
    void testTransferBudget()
    {
        using namespace std::chrono_literals;
        // A ready 26-image viewport need not wait thirteen timer ticks.
        for (std::size_t i = 0; i < 26; ++i)
            CPPUNIT_ASSERT(pdffast::TransferBudget::allows(i, 1ms));
        CPPUNIT_ASSERT(!pdffast::TransferBudget::allows(1, 4ms));
        CPPUNIT_ASSERT(!pdffast::TransferBudget::allows(64, 0ms));
        CPPUNIT_ASSERT(pdffast::TransferBudget::allows(0, 100ms)); // Always make progress.
    }
    void testCacheSelectiveInvalidation()
    {
        pdffast::Cache<int, int> cache(16);
        cache.put(1, 10, 4);
        cache.put(2, 20, 4);
        cache.put(3, 30, 4);
        const auto keys = cache.keys([](int key) { return key != 2; });
        CPPUNIT_ASSERT_EQUAL(std::size_t(2), keys.size());
        cache.eraseIf([](int key) { return key != 2; });
        CPPUNIT_ASSERT_EQUAL(std::size_t(4), cache.used());
        CPPUNIT_ASSERT_EQUAL(20, *cache.get(2));
        CPPUNIT_ASSERT(!cache.get(1));
        cache.put(4, 40, 12);
        CPPUNIT_ASSERT_EQUAL(std::size_t(16), cache.used());
    }
    void testVisibleCacheProtection()
    {
        constexpr std::size_t MiB = 1024 * 1024;
        pdffast::Cache<int, int> cache(16 * MiB);
        std::map<int, std::size_t> visible;
        for (int tile = 0; tile < 15; ++tile)
            visible.emplace(tile, MiB);
        visible.emplace(100, MiB / 4); // Visible preview also occupies the budget.
        cache.protect(visible);
        for (int tile = 0; tile < 15; ++tile)
            CPPUNIT_ASSERT(cache.put(tile, tile, MiB));
        CPPUNIT_ASSERT(cache.put(100, 100, MiB / 4));
        CPPUNIT_ASSERT_EQUAL(3 * MiB / 4, cache.spareForSpeculation());
        CPPUNIT_ASSERT(!cache.put(16, 16, MiB, true));
        for (int tile = 0; tile < 15; ++tile)
            CPPUNIT_ASSERT(cache.contains(tile));
        CPPUNIT_ASSERT_EQUAL(15 * MiB + MiB / 4, cache.used());

        pdffast::Cache<int, int> reserved(12);
        reserved.protect({ { 1, 4 }, { 2, 4 } });
        CPPUNIT_ASSERT(reserved.put(3, 3, 4, true));
        CPPUNIT_ASSERT(!reserved.put(5, 5, 5, true)); // Reserve unseen visible results.
        CPPUNIT_ASSERT(reserved.put(1, 1, 4));
        CPPUNIT_ASSERT(reserved.put(2, 2, 4));
        CPPUNIT_ASSERT(reserved.get(3)); // Recency must not override priority.
        reserved.protect({ { 1, 4 }, { 2, 4 }, { 4, 4 } });
        CPPUNIT_ASSERT(reserved.put(4, 4, 4));
        CPPUNIT_ASSERT(!reserved.contains(3));
        CPPUNIT_ASSERT(reserved.contains(1) && reserved.contains(2));
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), reserved.spareForSpeculation());
        reserved.protect({ { 4, 4 } }); // A later viewport releases old protection.
        CPPUNIT_ASSERT(reserved.put(5, 5, 4, true));
        CPPUNIT_ASSERT(reserved.contains(4));
        CPPUNIT_ASSERT(reserved.used() <= 12);
        // Existing page 4 still costs 4; the missing page needs another 9.
        // Together they exceed the 12-byte budget without redefining a resident cost.
        reserved.protect({ { 4, 4 }, { 7, 9 } });
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), reserved.spareForSpeculation());
        CPPUNIT_ASSERT(!reserved.put(6, 6, 1, true));
        CPPUNIT_ASSERT(reserved.contains(4));
        CPPUNIT_ASSERT(!reserved.contains(6));
        CPPUNIT_ASSERT(reserved.used() <= 12);
    }
    void testPreviewSurvivesOversizedWorkingSet()
    {
        constexpr std::size_t MiB = 1024 * 1024;
        const pdffast::TileKey preview{ 0, 256, 363, 0, 0, 256, 363 };
        const std::size_t previewCost = 256 * 363 * 4;
        pdffast::Cache<pdffast::TileKey, int> cache(16 * MiB);
        std::map<pdffast::TileKey, std::size_t> visible{ { preview, previewCost } };
        std::vector<pdffast::TileKey> tiles;
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x)
            {
                pdffast::TileKey tile{ 0, 2560, 3630, x * 512, y * 512, 512, 512 };
                tiles.push_back(tile);
                visible.emplace(tile, MiB);
            }
        cache.protect(visible, { { preview, previewCost } });
        CPPUNIT_ASSERT(cache.put(preview, 0, previewCost));
        for (const auto& tile : tiles)
        {
            CPPUNIT_ASSERT(cache.get(preview)); // Preview painted before sharp tiles.
            CPPUNIT_ASSERT(cache.put(tile, 1, MiB));
            for (const auto& painted : tiles)
                cache.get(painted);
            CPPUNIT_ASSERT(cache.contains(preview));
            CPPUNIT_ASSERT(cache.used() <= 16 * MiB);
        }
        CPPUNIT_ASSERT(std::any_of(tiles.begin(), tiles.end(), [&](const auto& tile) {
            return !cache.contains(tile);
        })); // Partial sharp coverage still has a preview behind every hole.
        CPPUNIT_ASSERT(!cache.put(tiles.front(), 2, 16 * MiB));
        CPPUNIT_ASSERT(cache.contains(preview));

        // Reserve all visible-page previews even when their results arrive late.
        pdffast::Cache<int, int> late(12);
        late.protect({ { 1, 4 }, { 2, 4 }, { 3, 4 }, { 100, 4 }, { 101, 4 } },
                     { { 100, 4 }, { 101, 4 } });
        for (int tile : { 1, 2, 3 })
            CPPUNIT_ASSERT(late.put(tile, tile, 4));
        CPPUNIT_ASSERT(late.used() <= 4);
        CPPUNIT_ASSERT(late.put(100, 100, 4));
        CPPUNIT_ASSERT(late.put(101, 101, 4));
        CPPUNIT_ASSERT(late.put(1, 1, 4));
        CPPUNIT_ASSERT(late.contains(100) && late.contains(101));
        CPPUNIT_ASSERT(!late.put(2, 2, 5));
        CPPUNIT_ASSERT(late.used() <= 12);
        late.protect({ { 5, 12 } }); // Leaving the viewport releases old previews.
        CPPUNIT_ASSERT(late.put(5, 5, 12));
        CPPUNIT_ASSERT(!late.contains(100) && !late.contains(101));
    }
    void testFallbackCoverage()
    {
        const pdffast::TileKey oldLeft{ 0, 1000, 1000, 0, 0, 500, 500 };
        const pdffast::TileKey oldRight{ 0, 1000, 1000, 500, 0, 500, 500 };
        const pdffast::TileKey gap{ 0, 2000, 2000, 1000, 0, 1000, 1000 };
        CPPUNIT_ASSERT(!pdffast::fallbackIntersects(oldLeft, gap)); // Boundary only.
        CPPUNIT_ASSERT(pdffast::fallbackIntersects(oldRight, gap));
        auto otherPage = gap;
        otherPage.page = 1;
        CPPUNIT_ASSERT(!pdffast::fallbackIntersects(oldRight, otherPage));
        // Once current-resolution coverage fills the last gap, selection must
        // neither paint nor touch any of the retained old-resolution entries.
        std::vector<pdffast::TileKey> missing{ gap };
        pdffast::Cache<pdffast::TileKey, int> cache(8);
        cache.put(oldLeft, 1, 4);
        cache.put(oldRight, 2, 4);
        const auto needed = cache.keys([&](const auto& key) {
            return std::any_of(missing.begin(), missing.end(), [&](const auto& hole) {
                return pdffast::fallbackIntersects(key, hole);
            });
        });
        CPPUNIT_ASSERT_EQUAL(std::size_t(1), needed.size());
        CPPUNIT_ASSERT(needed.front() == oldRight);
        missing.clear();
        CPPUNIT_ASSERT(cache.keys([&](const auto& key) {
            return std::any_of(missing.begin(), missing.end(), [&](const auto& hole) {
                return pdffast::fallbackIntersects(key, hole);
            });
        }).empty());
        // Coverage inspection must not promote either old cache entry.
        const pdffast::TileKey fresh{ 0, 2000, 2000, 0, 0, 512, 512 };
        cache.put(fresh, 3, 4);
        CPPUNIT_ASSERT(!cache.contains(oldLeft));
        CPPUNIT_ASSERT(cache.contains(oldRight));
    }
    void testCacheEviction()
    {
        pdffast::Cache<int, int> cache(10);
        cache.put(1, 11, 4);
        cache.put(2, 22, 4);
        CPPUNIT_ASSERT_EQUAL(11, *cache.get(1)); // Page 2 is now least recently used.
        cache.put(3, 33, 4);
        CPPUNIT_ASSERT(cache.contains(1));
        CPPUNIT_ASSERT(!cache.contains(2));
        CPPUNIT_ASSERT(cache.contains(3));
        CPPUNIT_ASSERT_EQUAL(std::size_t(8), cache.used());
        cache.clear();
        CPPUNIT_ASSERT_EQUAL(std::size_t(0), cache.used());
        CPPUNIT_ASSERT(!cache.get(1));
    }
    void testReplacementAndOversizedEntry()
    {
        pdffast::Cache<int, int> cache(10);
        cache.put(1, 11, 4);
        cache.put(2, 22, 4);
        cache.put(1, 12, 7);
        CPPUNIT_ASSERT(!cache.contains(2));
        CPPUNIT_ASSERT_EQUAL(std::size_t(7), cache.used());
        cache.put(1, 13, 11);
        CPPUNIT_ASSERT_EQUAL(12, *cache.get(1));
        cache.put(3, 33, 10);
        CPPUNIT_ASSERT(!cache.contains(1));
        CPPUNIT_ASSERT_EQUAL(std::size_t(10), cache.used());
        pdffast::Cache<int, int> empty(0);
        empty.put(1, 11, 1);
        CPPUNIT_ASSERT(!empty.contains(1));
    }
};
CPPUNIT_TEST_SUITE_REGISTRATION(CoreTest);
}
CPPUNIT_PLUGIN_IMPLEMENT();
