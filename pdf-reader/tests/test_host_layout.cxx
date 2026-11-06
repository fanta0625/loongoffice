/* SPDX-License-Identifier: MPL-2.0 */
#include <PdfHybridViewLayout.hxx>

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace sd::pdfhybrid;

namespace
{
void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

void check(const std::array<Extent, 3>& sizes, std::int64_t gap)
{
    const auto layout = makeLayout(sizes, gap);
    require(bool(layout), "valid page geometry rejected");
    const auto& pages = layout->pages;
    require(pages[1].x == 0 && pages[1].y == 0, "active edit origin changed");
    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto& page = pages[i];
        require(page.width == sizes[i].width && page.height == sizes[i].height,
                "page size changed");
        require(layout->workspace.contains(page.x, page.y)
                    && layout->workspace.contains(page.x + page.width - 1, page.y + page.height - 1),
                "page outside scroll extent");
        require(page.contains(page.x, page.y)
                    && !page.contains(page.x + page.width, page.y)
                    && !page.contains(page.x, page.y + page.height),
                "preview hit rectangle is not half-open");
        const auto centerDelta = 2 * page.x + page.width - sizes[1].width;
        require(centerDelta >= -1 && centerDelta <= 1, "page is not horizontally centered");
    }
    require(pages[0].y + pages[0].height == -gap, "upper page overlaps active page");
    require(pages[2].y == pages[1].height + gap, "lower page overlaps active page");
    require(!pages[0].contains(0, 0) && !pages[2].contains(0, 0),
            "preview intercepts active page input");
}
}

int main()
{
    try
    {
        check({ Extent{ 20000, 10000 }, Extent{ 20000, 10000 }, Extent{ 20000, 10000 } }, 1000);
        check({ Extent{ 18000, 9000 }, Extent{ 20000, 10000 }, Extent{ 24000, 12000 } }, 1000);
        check({ Extent{ 1, 1 }, Extent{ 10000000, 10000000 }, Extent{ 1, 10000000 } }, 10000000);
        for (std::int64_t n = 1; n < 300; ++n)
            check({ Extent{ 77 * n + 1, 31 * n }, Extent{ 29 * n, 107 * n + 1 },
                    Extent{ 53 * n, 19 * n + 1 } }, n);
        const std::array<Extent, 3> valid{ Extent{ 20, 10 }, Extent{ 20, 10 }, Extent{ 20, 10 } };
        require(!makeLayout(valid, 0) && !makeLayout(valid, -1), "invalid gap accepted");
        for (const auto bad : { std::int64_t(0), std::int64_t(-1), std::int64_t(10000001),
                               std::numeric_limits<std::int64_t>::max() })
        {
            auto invalid = valid;
            invalid[2].height = bad;
            require(!makeLayout(invalid), "invalid page height accepted");
        }
        std::cout << "Hybrid layout geometry checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
