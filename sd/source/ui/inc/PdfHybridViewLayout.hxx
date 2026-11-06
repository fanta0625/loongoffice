/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

// Geometry only: no model, window, renderer or Office dependency. The active
// page stays at (0, 0); native Draw input therefore needs no coordinate proxy.
namespace sd::pdfhybrid
{
struct Extent
{
    std::int64_t width;
    std::int64_t height;
};

struct Rect
{
    std::int64_t x;
    std::int64_t y;
    std::int64_t width;
    std::int64_t height;

    bool contains(std::int64_t px, std::int64_t py) const
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
    bool operator==(const Rect& other) const
    {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

struct Layout
{
    std::array<Rect, 3> pages;
    Rect workspace;

    bool operator==(const Layout& other) const
    {
        return pages == other.pages && workspace == other.workspace;
    }
};

inline std::optional<Layout> makeLayout(const std::array<Extent, 3>& sizes,
                                      std::int64_t gap = 1000)
{
    // Bound arithmetic before passing geometry into VCL's platform-sized Long.
    constexpr std::int64_t limit = 10000000;
    if (gap <= 0 || gap > limit)
        return std::nullopt;
    for (const auto& size : sizes)
        if (size.width <= 0 || size.height <= 0 || size.width > limit || size.height > limit)
            return std::nullopt;
    const auto& active = sizes[1];
    Layout result{ { Rect{ (active.width - sizes[0].width) / 2, -gap - sizes[0].height,
                           sizes[0].width, sizes[0].height },
                     Rect{ 0, 0, active.width, active.height },
                     Rect{ (active.width - sizes[2].width) / 2, active.height + gap,
                           sizes[2].width, sizes[2].height } },
                   {} };
    const auto left = std::min({ result.pages[0].x, std::int64_t(0), result.pages[2].x });
    const auto right = std::max({ result.pages[0].x + sizes[0].width, active.width,
                                 result.pages[2].x + sizes[2].width });
    const auto marginX = std::max({ sizes[0].width, active.width, sizes[2].width });
    result.workspace = { left - marginX, result.pages[0].y - gap,
                         right - left + 2 * marginX,
                         result.pages[2].y + sizes[2].height - result.pages[0].y + 2 * gap };
    return result;
}
}
