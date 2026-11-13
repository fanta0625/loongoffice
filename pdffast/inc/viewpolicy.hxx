/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "geometry.hxx"
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace pdffast
{
// A single, zoom-independent image serves both the page and small thumbnails.
inline TileKey pagePreview(int page, double width, double height)
{
    if (width <= 0 || height <= 0)
        return {};
    const double scale = std::min(256.0 / width, 512.0 / height);
    const int w = std::max(1, int(width * scale));
    const int h = std::max(1, int(height * scale));
    return { page, w, h, 0, 0, w, h };
}

// Scrolling at one scale is immediate. Only successive resolution changes
// delay expensive tiles; repainting a delivered preview must not reset this.
class ZoomSettle
{
    using Clock = std::chrono::steady_clock;
    static constexpr auto PageSettle = std::chrono::milliseconds(120);
    int m_page = -1;
    int m_width = 0;
    int m_height = 0;
    Clock::time_point m_until{};

public:
    void update(int page, int width, int height, Clock::time_point now)
    {
        if (page == m_page && (width != m_width || height != m_height))
            m_until = now + std::chrono::milliseconds(80);
        else if (page != m_page)
            // A page switch must show its bounded preview before we start
            // filling the whole viewport with expensive high-resolution tiles.
            // This is especially important for large vector pages on slower
            // CPUs: several visible tiles can otherwise monopolize the PDFium
            // worker while the user is still seeking.
            m_until = now + PageSettle;
        m_page = page;
        m_width = width;
        m_height = height;
    }
    bool pending(Clock::time_point now) const { return now < m_until; }
};

// Select one old resolution, never a stack of all cached zoom levels. The
// caller clips it to missing current-resolution coverage before painting.
inline std::pair<int, int> fallbackLevel(const std::vector<TileKey>& candidates, int width,
                                         int height)
{
    std::pair<int, int> result{};
    double best = std::numeric_limits<double>::infinity();
    if (width <= 0 || height <= 0)
        return result;
    for (const auto& key : candidates)
    {
        if (key.pageWidth <= 0 || key.pageHeight <= 0)
            continue;
        const double distance = std::abs(std::log(double(key.pageWidth) / width))
                                + std::abs(std::log(double(key.pageHeight) / height));
        if (distance < best)
        {
            best = distance;
            result = { key.pageWidth, key.pageHeight };
        }
    }
    return result;
}
}
