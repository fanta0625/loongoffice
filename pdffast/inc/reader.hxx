/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once
#include <vcl/wrkwin.hxx>
#include <chrono>
#include <cstddef>

namespace pdffast
{
VclPtr<WorkWindow> createReader(const OUString& url, std::size_t cacheBudget,
                               std::chrono::steady_clock::time_point started);
}
