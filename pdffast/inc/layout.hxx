/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "geometry.hxx"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace pdffast
{
// PDF points, independent of zoom and native window coordinates. Unknown pages
// use the first page's size. A Fenwick tree avoids quadratic layout updates as
// page sizes arrive; no all-page PDF metadata scan is needed before first paint.
class Layout
{
    PageSize m_estimate;
    std::vector<PageSize> m_sizes;
    std::vector<double> m_delta;
    double m_maxWidth = 0;

    double prefix(int count) const
    {
        double value = 0;
        for (std::size_t i = count; i; i -= i & (~i + 1))
            value += m_delta[i];
        return value;
    }

public:
    static constexpr double Gap = 12;
    void reset(int count, PageSize estimate)
    {
        if (count <= 0 || count > 1000000 || !estimate.valid())
            throw std::invalid_argument("Invalid PDF page geometry/count");
        m_estimate = estimate;
        m_sizes.assign(count, {});
        m_delta.assign(std::size_t(count) + 1, 0);
        m_maxWidth = estimate.width;
        set(0, estimate);
    }
    int count() const { return m_sizes.size(); }
    bool known(int page) const { return m_sizes.at(page).valid(); }
    PageSize size(int page) const { return known(page) ? m_sizes[page] : m_estimate; }
    void set(int page, PageSize value)
    {
        if (!value.valid())
            throw std::invalid_argument("Invalid PDF page size");
        const double delta = value.height - size(page).height;
        m_sizes.at(page) = value;
        m_maxWidth = std::max(m_maxWidth, value.width);
        for (std::size_t i = std::size_t(page) + 1; i < m_delta.size(); i += i & (~i + 1))
            m_delta[i] += delta;
    }
    double top(int page) const { return Gap + page * (m_estimate.height + Gap) + prefix(page); }
    double height() const { return top(count()); }
    // Keep even a short final page top-alignable. Navigation, the scroll range
    // and pageAt() must agree on the page that F2 will prepare for editing.
    double scrollHeight(double viewportHeight) const
    {
        return count() ? std::max(height(), top(count() - 1) + viewportHeight + Gap)
                       : viewportHeight;
    }
    double width() const { return m_maxWidth + 2 * Gap; }
    int pageAt(double y) const
    {
        if (!count())
            return 0;
        int lo = 0, hi = count();
        while (lo + 1 < hi)
        {
            int mid = lo + (hi - lo) / 2;
            if (top(mid) <= y)
                lo = mid;
            else
                hi = mid;
        }
        return lo;
    }
};
}
