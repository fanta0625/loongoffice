/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "types.hxx"
#include "demand.hxx"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>
#include <thread>

namespace pdffast
{
// Worker never constructs a VCL Bitmap, touches a window, or posts callbacks.
// The UI drains bounded, owning results. Joining requires no SolarMutex and no
// UI event dispatch. Raster work cooperatively cancels at PDFium checkpoints;
// page parsing and form drawing can still block until their calls return.
class Session final
{
    std::shared_ptr<Source> m_source;
    std::shared_ptr<const std::vector<sal_uInt8>> m_memory;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    Demand m_demand;
    std::deque<Result> m_results;
    std::optional<OString> m_password;
    std::size_t m_resultBytes = 0;
    bool m_active = true;
    std::thread m_worker;

    void run() noexcept;
    bool publish(Result result);

public:
    explicit Session(const OUString& url);
    explicit Session(const BinaryDataContainer& bytes);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void unlock(const OString& password);
    void request(std::vector<Request> requests);
    // The UI checks its time/quantity budget between individual transfers.
    std::optional<Result> takeResult();
    bool hasResults();
    bool idle();
    std::shared_ptr<Source> source() const { return m_source; }
};
}
