/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <limits>
#include <optional>
#include <string_view>

namespace pdfi
{
// Existing options keep their original meaning. Page= is deliberately a
// single strict 1-based page, not a range language or an extraction operation.
template <typename Char>
std::optional<int> selectedPage(std::basic_string_view<Char> option, int pageCount)
{
    constexpr char prefix[] = "Page=";
    bool matches = option.size() >= 5;
    for (unsigned i = 0; matches && i < 5; ++i)
        matches = option[i] == Char(prefix[i]);
    if (!matches)
        return 0; // Whole document, legacy option handling.
    if (option.size() == 5)
        return std::nullopt;
    int page = 0;
    for (const auto c : option.substr(5))
    {
        if (c < Char('0') || c > Char('9'))
            return std::nullopt;
        const int digit = c - Char('0');
        if (page > (std::numeric_limits<int>::max() - digit) / 10)
            return std::nullopt;
        page = page * 10 + digit;
    }
    return page > 0 && page <= pageCount ? std::optional<int>(page) : std::nullopt;
}
}
