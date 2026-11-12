/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <optional>
#include <string_view>

namespace pdfi
{
struct PageSelection
{
    int first;
    int last;
};

// Private option used by the deferred Draw importer. Unrelated options retain
// their existing meaning, including an unrestricted import by default.
inline std::optional<PageSelection> selectPages(std::string_view options, int pageCount)
{
    if (pageCount <= 0)
        return std::nullopt;
    constexpr std::string_view prefix = "LoongOfficePdfPage=";
    if (!options.starts_with(prefix))
        return PageSelection{ 1, pageCount };
    options.remove_prefix(prefix.size());
    if (options.empty())
        return std::nullopt;
    int page = 0;
    for (char digit : options)
    {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        const int value = digit - '0';
        // Bound parsing by pageCount, without overflowing even for long input.
        if (value > pageCount || page > (pageCount - value) / 10)
            return std::nullopt;
        page = page * 10 + value;
    }
    if (!page)
        return std::nullopt;
    return PageSelection{ page, page };
}
}
