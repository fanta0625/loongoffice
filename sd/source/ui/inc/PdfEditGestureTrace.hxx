// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

namespace sd
{
inline void tracePdfEditGesture(const char* pEvent, int nDetail = 0)
{
    static const bool bEnabled = std::getenv("LO_PDF_EDIT_GESTURE_TRACE") != nullptr;
    if (!bEnabled)
        return;
    const auto nTime = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stderr, "pdf-gesture unix_us=%lld thread=%zu event=%s detail=%d\n",
                 static_cast<long long>(nTime), std::hash<std::thread::id>{}(std::this_thread::get_id()),
                 pEvent, nDetail);
    std::fflush(stderr);
}
}
