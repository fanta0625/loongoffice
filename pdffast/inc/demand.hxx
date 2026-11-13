/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once
#include "geometry.hxx"
#include <deque>
#include <optional>
#include <set>
#include <vector>

namespace pdffast
{
// Caller supplies synchronization. Input order is priority order; work identity
// survives viewport changes. Queued results and the in-flight task are not rerun.
class Demand
{
    std::set<Request> m_needed;
    std::deque<Request> m_pending;
    std::vector<Request> m_running;

public:
    void update(const std::vector<Request>& requests, const std::set<Request>& ready)
    {
        m_needed.clear();
        m_pending.clear();
        for (const auto& request : requests)
            if (m_needed.insert(request).second && !ready.count(request)
                && std::find(m_running.begin(), m_running.end(), request) == m_running.end())
                m_pending.push_back(request);
    }
    bool needs(const Request& request) const { return m_needed.count(request) != 0; }
    bool empty() const { return m_pending.empty(); }
    Request next()
    {
        Request request = m_pending.front();
        m_pending.pop_front();
        m_running = { request };
        return request;
    }
    std::vector<Request> nextBatch(std::size_t maxChildren = 4)
    {
        const auto first = next();
        auto area = first.tile;
        const std::size_t limit = std::clamp<std::size_t>(maxChildren, 1, first.prefetch ? 2 : 4);
        while (!first.metadata() && !m_pending.empty() && m_running.size() < limit)
        {
            const auto& candidate = m_pending.front();
            if (candidate.metadata() || candidate.prefetch != first.prefetch)
                break;
            const auto merged = mergeRegion(area, candidate.tile);
            if (!merged)
                break;
            area = *merged;
            m_running.push_back(candidate);
            m_pending.pop_front();
        }
        return m_running;
    }
    bool hasForeground() const
    {
        return std::any_of(m_pending.begin(), m_pending.end(),
                           [](const Request& r) { return !r.prefetch; });
    }
    void finish(bool retry = false)
    {
        if (retry)
            for (const auto& request : m_running)
                if (needs(request)
                    && std::find(m_pending.begin(), m_pending.end(), request) == m_pending.end())
                    m_pending.push_back(request);
        m_running.clear();
    }
};
}
