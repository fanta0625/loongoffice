/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include "session.hxx"
#include "input.hxx"
#include "layout.hxx"
#include <osl/file.hxx>
#include <tools/stream.hxx>
#include <vcl/filter/PDFiumLibrary.hxx>
#include <vcl/pdf/PDFBitmapType.hxx>
#include <vcl/pdf/PDFErrorType.hxx>
#include <sal/log.hxx>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>

namespace pdffast
{
namespace
{
constexpr std::size_t ResultBudget = 16 * 1024 * 1024;

Pixels copyPixels(vcl::pdf::PDFiumBitmap& bitmap, const TileKey& key, const TileKey& area)
{
    if (bitmap.getWidth() != area.width || bitmap.getHeight() != area.height)
        throw std::runtime_error("The engine could not allocate the requested tile size");
    const auto format = bitmap.getFormat();
    const int channels = format == vcl::pdf::PDFBitmapType::BGR ? 3 : 4;
    if (format != vcl::pdf::PDFBitmapType::BGR && format != vcl::pdf::PDFBitmapType::BGRx
        && format != vcl::pdf::PDFBitmapType::BGRA)
        throw std::runtime_error("Unsupported PDF bitmap format");
    if (!bitmap.getBuffer() || bitmap.getStride() < area.width * channels)
        throw std::runtime_error("Invalid PDF bitmap buffer");
    Pixels out;
    out.width = key.width;
    out.height = key.height;
    out.stride = key.width * 3;
    out.data.resize(std::size_t(out.stride) * out.height);
    for (int y = 0; y < out.height; ++y)
    {
        const auto* src = bitmap.getBuffer() + std::size_t(y + key.y - area.y) * bitmap.getStride()
                          + std::size_t(key.x - area.x) * channels;
        auto* dst = out.data.data() + std::size_t(y) * out.stride;
        for (int x = 0; x < out.width; ++x)
            std::memcpy(dst + x * 3, src + x * channels, 3);
    }
    return out;
}
}

Session::Session(const OUString& url)
    : m_source(std::make_shared<Source>())
{
    m_source->url = url;
    m_worker = std::thread([this] { run(); });
}

Session::Session(const BinaryDataContainer& bytes)
    : m_source(std::make_shared<Source>())
{
    m_source->bytes = bytes;
    m_memory = bytes.getDataShared();
    if (!m_memory || m_memory->empty()
        || m_memory->size() > std::size_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Invalid memory PDF size");
    m_worker = std::thread([this] { run(); });
}

Session::~Session()
{
    {
        std::lock_guard guard(m_mutex);
        *m_source->stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

void Session::unlock(const OString& password)
{
    std::lock_guard guard(m_mutex);
    m_password = password;
    m_wake.notify_all();
}

void Session::request(std::vector<Request> requests)
{
    std::lock_guard guard(m_mutex);
    std::set<Request> ready;
    for (const auto& result : m_results)
        if (result.requested)
            ready.insert({ result.page, result.tile });
    m_demand.update(requests, ready);
    for (auto it = m_results.begin(); it != m_results.end();)
        if (it->requested && !m_demand.needs({ it->page, it->tile }))
        {
            m_resultBytes -= it->pixels.data.size();
            it = m_results.erase(it);
        }
        else
            ++it;
    m_wake.notify_all();
}

std::optional<Result> Session::takeResult()
{
    std::lock_guard guard(m_mutex);
    if (m_results.empty())
        return {};
    m_resultBytes -= m_results.front().pixels.data.size();
    Result result = std::move(m_results.front());
    m_results.pop_front();
    m_wake.notify_all();
    return result;
}

bool Session::hasResults()
{
    std::lock_guard guard(m_mutex);
    return !m_results.empty();
}

bool Session::idle()
{
    std::lock_guard guard(m_mutex);
    return !m_active && m_demand.empty() && m_results.empty();
}

bool Session::publish(Result result)
{
    std::unique_lock guard(m_mutex);
    m_wake.wait(guard, [&] {
        return *m_source->stop
               || (result.requested && !m_demand.needs({ result.page, result.tile }))
               || m_resultBytes + result.pixels.data.size() <= ResultBudget;
    });
    if (*m_source->stop)
        return false;
    if (result.requested && !m_demand.needs({ result.page, result.tile }))
        return true;
    m_resultBytes += result.pixels.data.size();
    m_results.push_back(std::move(result));
    return true;
}

void Session::run() noexcept
{
    try
    {
        if (!m_memory)
            m_source->input = openInput(m_source->url, m_source->stop);
        if (*m_source->stop)
            return;
        // Keep initialization, calls, and destruction under the same process-local
        // engine lock. Pause callbacks may take m_mutex; callers must never
        // hold m_mutex while acquiring engineMutex (UI demand updates do not).
        auto engine = [&] {
            std::lock_guard guard(m_source->engineMutex);
            return vcl::pdf::PDFiumLibrary::get();
        }();
        if (!engine)
            throw std::runtime_error("PDFium is unavailable");
        auto destroyDocument = [source = m_source](vcl::pdf::PDFiumDocument * value)
        {
            std::lock_guard guard(source->engineMutex);
            delete value;
        };
        std::unique_ptr<vcl::pdf::PDFiumDocument, decltype(destroyDocument)> document(
            nullptr, destroyDocument);
        auto destroyPage = [source = m_source](vcl::pdf::PDFiumPage * value)
        {
            std::lock_guard guard(source->engineMutex);
            delete value;
        };
        // One reusable page handle bounds residency and avoids reopening a page
        // for each tile. Declared after document so destruction order is safe.
        std::unique_ptr<vcl::pdf::PDFiumPage, decltype(destroyPage)> cachedPage(nullptr,
                                                                                destroyPage);
        int cachedPageIndex = -1;
        OString password;
        int count = 0;
        std::size_t batchLimit = 2;
        while (!*m_source->stop)
        {
            if (!document)
            {
                Result opened;
                {
                    std::lock_guard guard(m_source->engineMutex);
                    document.reset(m_source->input
                                       ? engine->openDocument(m_source->input, password).release()
                                       : engine
                                             ->openDocument(m_memory->data(),
                                                            static_cast<int>(m_memory->size()),
                                                            password)
                                             .release());
                    if (!document)
                    {
                        if (engine->getLastErrorCode() != vcl::pdf::PDFErrorType::Password)
                            throw std::runtime_error("Cannot parse the PDF document");
                        opened.kind = Result::Kind::Password;
                    }
                    else
                    {
                        count = document->getPageCount();
                        const auto first
                            = count > 0 ? document->getPageSize(0) : basegfx::B2DSize();
                        opened.size = { first.getWidth(), first.getHeight() };
                        if (count <= 0 || count > 1000000 || !opened.size.valid())
                            throw std::runtime_error("Invalid PDF page count or first page size");
                        opened.kind = Result::Kind::Opened;
                        opened.pageCount = count;
                    }
                }
                if (!publish(std::move(opened)))
                    break;
                if (!document)
                {
                    std::unique_lock guard(m_mutex);
                    m_active = false;
                    m_wake.wait(guard, [&] { return *m_source->stop || m_password.has_value(); });
                    if (*m_source->stop)
                        break;
                    password = std::move(*m_password);
                    m_password.reset();
                    m_active = true;
                    continue;
                }
            }
            std::vector<Request> batch;
            {
                std::unique_lock guard(m_mutex);
                m_active = false;
                m_wake.wait(guard, [&] { return *m_source->stop || !m_demand.empty(); });
                if (*m_source->stop)
                    break;
                batch = m_demand.nextBatch(batchLimit);
                m_active = true;
            }
            const auto cancelled = [&] {
                if (*m_source->stop)
                    return true;
                std::lock_guard guard(m_mutex);
                return !std::any_of(batch.begin(), batch.end(), [&](const Request& r) {
                    return m_demand.needs(r);
                }) || (batch.front().prefetch && m_demand.hasForeground());
            };
            const auto renderStarted = std::chrono::steady_clock::now();
            std::vector<Result> results;
            bool aborted = false;
            try
            {
                const auto& request = batch.front();
                if (!request.metadata() && cachedPageIndex != request.page)
                {
                    cachedPage.reset();
                    cachedPageIndex = -1;
                }
                std::lock_guard guard(m_source->engineMutex);
                if (m_source->input)
                    m_source->input->validate();
                if (request.page < 0 || request.page >= count)
                    throw std::runtime_error("PDF page index out of range");
                const auto size = document->getPageSize(request.page);
                const PageSize pageSize{ size.getWidth(), size.getHeight() };
                if (!pageSize.valid())
                    throw std::runtime_error("Invalid PDF page geometry");
                if (request.metadata())
                {
                    Result result;
                    result.kind = Result::Kind::Page;
                    result.requested = true;
                    result.page = request.page;
                    result.size = pageSize;
                    results.push_back(std::move(result));
                }
                else
                {
                    TileKey area = request.tile;
                    for (std::size_t i = 0; i < batch.size(); ++i)
                    {
                        const auto& key = batch[i].tile;
                        if (key.page != request.page || key.pageWidth <= 0 || key.pageHeight <= 0
                            || key.pageWidth > 1000000 || key.pageHeight > 1000000 || key.x < 0
                            || key.y < 0 || key.width <= 0 || key.height <= 0 || key.width > 1024
                            || key.height > 1024 || key.x > key.pageWidth - key.width
                            || key.y > key.pageHeight - key.height)
                            throw std::runtime_error("Invalid PDF tile geometry");
                        if (i)
                        {
                            const auto merged = mergeRegion(area, key);
                            if (!merged)
                                throw std::runtime_error("Invalid PDF region merge");
                            area = *merged;
                        }
                    }
                    if (!cachedPage)
                    {
                        cachedPage.reset(document->openPage(request.page).release());
                        cachedPageIndex = request.page;
                    }
                    area = tileWithGutter(area);
                    int width = area.width, height = area.height;
                    auto bitmap = engine->createBitmap(width, height, 0);
                    if (!cachedPage || !bitmap)
                        throw std::runtime_error("Cannot render PDF page");
                    bitmap->fillRect(0, 0, width, height, 0xffffffff);
                    aborted = !bitmap->renderPageBitmapCancelable(document.get(), cachedPage.get(),
                                                                  -area.x, -area.y, area.pageWidth,
                                                                  area.pageHeight, cancelled);
                    if (!aborted)
                    {
                        if (m_source->input)
                            m_source->input->validate();
                        for (const auto& child : batch)
                        {
                            Result result;
                            result.kind = Result::Kind::Tile;
                            result.requested = true;
                            result.page = child.page;
                            result.tile = child.tile;
                            result.size = pageSize;
                            result.pixels = copyPixels(*bitmap, tileWithGutter(child.tile), area);
                            results.push_back(std::move(result));
                        }
                    }
                }
            }
            catch (const std::exception& error)
            {
                results.clear();
                for (const auto& child : batch)
                {
                    Result result;
                    result.requested = true;
                    result.page = child.page;
                    result.tile = child.tile;
                    result.message = OUString::fromUtf8(error.what());
                    results.push_back(std::move(result));
                }
            }
            const auto elapsed = std::chrono::steady_clock::now() - renderStarted;
            if (!batch.front().metadata())
                batchLimit = aborted || elapsed > std::chrono::milliseconds(12)
                                 ? 1
                                 : elapsed < std::chrono::milliseconds(4) ? 4 : 2;
            SAL_INFO("pdffast", "PDF region children="
                                    << batch.size() << " aborted=" << aborted << " in "
                                    << std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - renderStarted)
                                           .count()
                                    << " us"
                                    << " page=" << batch.front().page
                                    << " prefetch=" << batch.front().prefetch
                                    << " page_width=" << batch.front().tile.pageWidth
                                    << " page_height=" << batch.front().tile.pageHeight);
            for (auto& result : results)
                if (!publish(std::move(result)))
                    break;
            {
                std::lock_guard guard(m_mutex);
                m_demand.finish(aborted); // Retry preempted background work after visible tasks.
            }
        }
        cachedPage.reset();
        document.reset();
    }
    catch (const std::exception& error)
    {
        Result result;
        result.message = OUString::fromUtf8(error.what());
        publish(std::move(result));
    }
    catch (...)
    {
        SAL_WARN("pdffast", "Unexpected PDF worker failure");
        Result result;
        result.message = u"Cannot open the PDF document"_ustr;
        publish(std::move(result));
    }
    std::lock_guard guard(m_mutex);
    m_active = false;
    m_demand.update({}, {});
    m_demand.finish();
}
}
