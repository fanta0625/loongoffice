/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <list>
#include <iterator>
#include <map>
#include <cstddef>
#include <utility>
#include <vector>

namespace pdffast
{
// UI-thread-only cache. Editable page models never enter this cache.
template <typename Key, typename Value> class Cache
{
    struct Entry
    {
        Key key;
        Value value;
        std::size_t cost;
        bool speculative;
    };
    std::list<Entry> m_lru;
    std::map<Key, typename std::list<Entry>::iterator> m_index;
    std::size_t m_budget;
    std::size_t m_used = 0;
    std::map<Key, std::size_t> m_protected;
    std::map<Key, std::size_t> m_previews;

public:
    explicit Cache(std::size_t budget) : m_budget(budget) {}
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    std::size_t used() const { return m_used; }
    void clear() { m_index.clear(); m_lru.clear(); m_used = 0; m_protected.clear(); m_previews.clear(); }
    bool contains(const Key& key) const { return m_index.find(key) != m_index.end(); }
    const Value* get(const Key& key)
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return nullptr;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        return &it->second->value;
    }
    template <typename Predicate> void eraseIf(Predicate predicate)
    {
        for (auto it = m_lru.begin(); it != m_lru.end();)
            if (predicate(it->key))
            {
                m_used -= it->cost;
                m_index.erase(it->key);
                it = m_lru.erase(it);
            }
            else
                ++it;
    }
    template <typename Predicate> std::vector<Key> keys(Predicate predicate) const
    {
        std::vector<Key> result;
        for (const auto& entry : m_lru)
            if (predicate(entry.key))
                result.push_back(entry.key);
        return result;
    }
    // Reserve missing visible entries too: speculation must not consume the
    // space needed by visible results that have not reached the UI yet.
    // Previews are the minimum readable representation. Unlike ordinary visible
    // tiles, they cannot be evicted when the visible working set exceeds budget.
    void protect(std::map<Key, std::size_t> entries,
                 std::map<Key, std::size_t> previews = {})
    {
        m_protected = std::move(entries);
        m_previews = std::move(previews);
    }
    std::size_t spareForSpeculation() const
    {
        std::size_t reserved = 0;
        for (const auto& entry : m_protected)
        {
            if (entry.second > m_budget - reserved)
                return 0;
            reserved += entry.second;
        }
        return m_budget - reserved;
    }
    bool put(const Key& key, Value value, std::size_t cost, bool speculative = false)
    {
        if (cost > m_budget)
            return false;
        std::size_t reservedMissing = 0;
        if (!speculative)
        {
            std::size_t reservedPreviews = 0;
            const auto incomingPreview = m_previews.find(key);
            for (auto it = m_previews.begin(); it != m_previews.end(); ++it)
            {
                if (it == incomingPreview)
                    continue;
                if (it->second > m_budget - reservedPreviews)
                    return false;
                reservedPreviews += it->second;
                if (!contains(it->first))
                    reservedMissing += it->second;
            }
            // Reject before touching the cache, including replacements. Reserve
            // previews that are still rendering, not only those already resident.
            if (cost > m_budget - reservedPreviews)
                return false;
        }
        if (speculative)
        {
            for (const auto& [visible, bytes] : m_protected)
                if (!contains(visible))
                {
                    if (bytes > m_budget - reservedMissing)
                        return false;
                    reservedMissing += bytes;
                }
            if (cost > m_budget - reservedMissing)
                return false;
            // Admission must be decided before changing the existing cache.
            std::size_t pinned = 0;
            for (const auto& entry : m_lru)
                if (m_protected.count(entry.key))
                {
                    if (entry.cost > m_budget - pinned)
                        return false;
                    pinned += entry.cost;
                }
            if (pinned > m_budget - reservedMissing - cost)
                return false;
        }
        auto old = m_index.find(key);
        if (old != m_index.end())
        {
            m_used -= old->second->cost;
            m_lru.erase(old->second);
            m_index.erase(old);
        }
        while (!m_lru.empty() && m_used > m_budget - cost - reservedMissing)
        {
            auto victim = m_lru.end();
            // Speculation is always discarded first, regardless of paint recency.
            for (auto it = m_lru.end(); it != m_lru.begin();)
            {
                --it;
                if (!m_protected.count(it->key) && !m_previews.count(it->key)
                    && (victim == m_lru.end() || it->speculative))
                {
                    victim = it;
                    if (it->speculative)
                        break;
                }
            }
            if (victim == m_lru.end())
            {
                // Only a foreground working set larger than the budget can
                // reach this case. Keep the budget hard and degrade to preview.
                if (speculative)
                    return false;
                for (auto it = m_lru.end(); it != m_lru.begin();)
                {
                    --it;
                    if (!m_previews.count(it->key))
                    {
                        victim = it;
                        break;
                    }
                }
                if (victim == m_lru.end())
                    return false;
            }
            m_used -= victim->cost;
            m_index.erase(victim->key);
            m_lru.erase(victim);
        }
        m_lru.push_front({ key, std::move(value), cost, speculative });
        m_index.emplace(key, m_lru.begin());
        m_used += cost;
        return true;
    }
};
}
