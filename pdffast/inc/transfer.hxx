/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once
#include <chrono>
#include <cstddef>
namespace pdffast
{
struct TransferBudget
{
    static constexpr std::size_t MaxItems = 64;
    static constexpr auto Slice = std::chrono::milliseconds(4);
    static bool allows(std::size_t items, std::chrono::steady_clock::duration elapsed)
    {
        return items < MaxItems && (items == 0 || elapsed < Slice);
    }
};
}
