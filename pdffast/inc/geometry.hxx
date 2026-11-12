/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once
#include <cmath>
#include <cstdint>
#include <tuple>
#include <algorithm>
#include <optional>

namespace pdffast
{
struct PageSize
{
    double width = 0;
    double height = 0;
    bool valid() const
    {
        return std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0
               && width <= 1000000 && height <= 1000000;
    }
};

struct TileKey
{
    int page = 0;
    int pageWidth = 0;
    int pageHeight = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    auto fields() const { return std::tie(page, pageWidth, pageHeight, x, y, width, height); }
    bool operator<(const TileKey& other) const { return fields() < other.fields(); }
    bool operator==(const TileKey& other) const { return fields() == other.fields(); }
};
// Compare normalized page areas without rounding to screen pixels. Used only
// for old-resolution fallback; adjacent tile boundaries do not overlap.
inline bool fallbackIntersects(const TileKey& old, const TileKey& gap)
{
    if (old.page != gap.page || old.pageWidth <= 0 || old.pageHeight <= 0
        || gap.pageWidth <= 0 || gap.pageHeight <= 0)
        return false;
    return std::int64_t(old.x) * gap.pageWidth < std::int64_t(gap.x + gap.width) * old.pageWidth
        && std::int64_t(gap.x) * old.pageWidth < std::int64_t(old.x + old.width) * gap.pageWidth
        && std::int64_t(old.y) * gap.pageHeight < std::int64_t(gap.y + gap.height) * old.pageHeight
        && std::int64_t(gap.y) * old.pageHeight < std::int64_t(old.y + old.height) * gap.pageHeight;
}

struct Request
{
    int page = 0;
    TileKey tile;
    bool prefetch = false; // Priority only; intentionally excluded from identity.
    bool metadata() const { return tile.width == 0; }
    bool operator<(const Request& other) const
    {
        return std::tie(page, tile) < std::tie(other.page, other.tile);
    }
    bool operator==(const Request& other) const
    {
        return page == other.page && tile == other.tile;
    }
};

// Merge only adjacent rectangles with no holes. Preserve child tile identities
// for demand reuse, upload and LRU accounting; never merge a full-page preview.
inline std::optional<TileKey> mergeRegion(const TileKey& a, const TileKey& b)
{
    if (a.width <= 0 || b.width <= 0 || a.height <= 0 || b.height <= 0 || a.page != b.page
        || a.pageWidth != b.pageWidth || a.pageHeight != b.pageHeight
        || (a.width == a.pageWidth && a.height == a.pageHeight)
        || (b.width == b.pageWidth && b.height == b.pageHeight))
        return {};
    TileKey area = a;
    area.x = std::min(a.x, b.x);
    area.y = std::min(a.y, b.y);
    area.width = std::max(a.x + a.width, b.x + b.width) - area.x;
    area.height = std::max(a.y + a.height, b.y + b.height) - area.y;
    const bool adjacent = (a.y == b.y && a.height == b.height
                           && (a.x + a.width == b.x || b.x + b.width == a.x))
                       || (a.x == b.x && a.width == b.width
                           && (a.y + a.height == b.y || b.y + b.height == a.y));
    if (!adjacent || area.width > 2048 || area.height > 1024
        || std::int64_t(area.width) * area.height > 1024 * 1024)
        return {};
    return area;
}

}
