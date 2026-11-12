/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include "input.hxx"
#include "cache.hxx"
#include <osl/file.hxx>
#include <osl/file.h>
#include <cstring>
#include <limits>
#include <chrono>
#include <comphelper/scopeguard.hxx>
#include <sal/log.hxx>

namespace pdffast
{
namespace
{
// All methods run under the session engine lock after initial construction.
class FileInput final : public Input
{
    OUString m_url;
    oslFileHandle m_file = nullptr;
    sal_uInt64 m_size = 0;
    TimeValue m_modified{};
    std::shared_ptr<std::atomic<bool>> m_stop;
    Cache<sal_uInt64, std::vector<unsigned char>> m_blocks{ 2 * 1024 * 1024 };
    BinaryDataContainer m_frozen;
    static constexpr std::size_t Block = 64 * 1024;
    bool m_failed = false;
    std::uint64_t m_reads = 0, m_checks = 0, m_misses = 0, m_bytes = 0;
    std::uint64_t m_readUs = 0, m_checkUs = 0;
public:
    FileInput(OUString url, std::shared_ptr<std::atomic<bool>> stop)
        : m_url(std::move(url)), m_stop(std::move(stop))
    {
        osl::DirectoryItem item;
        osl::FileStatus status(osl_FileStatus_Mask_FileSize | osl_FileStatus_Mask_ModifyTime);
        if (osl::DirectoryItem::get(m_url, item) != osl::FileBase::E_None
            || item.getFileStatus(status) != osl::FileBase::E_None)
            throw std::runtime_error("Cannot read PDF file information");
        m_size = status.getFileSize();
        m_modified = status.getModifyTime();
        if (!m_size || m_size > std::numeric_limits<int>::max())
            throw std::runtime_error("The PDF is empty or exceeds the editing engine's 2 GiB limit");
        if (osl_openFile(m_url.pData, &m_file, osl_File_OpenFlag_Read) != osl_File_E_None)
            throw std::runtime_error("Cannot open PDF file");
        // Do not throw after acquiring the handle: validation/signature checking
        // is performed by openInput after shared ownership has been established.
    }
    ~FileInput() override
    {
        SAL_INFO("pdffast", "input-summary reads=" << m_reads << " checks=" << m_checks
            << " misses=" << m_misses << " bytes=" << m_bytes << " read_us=" << m_readUs
            << " check_us=" << m_checkUs);
        if (m_file)
            osl_closeFile(m_file);
    }
    sal_uInt64 size() const override { return m_size; }
    void validate() override
    {
        const auto started = std::chrono::steady_clock::now();
        ++m_checks;
        comphelper::ScopeGuard timing([&] {
            m_checkUs += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
        });
        if (*m_stop || m_failed)
            throw std::runtime_error("The PDF source is no longer available");
        if (!m_frozen.isEmpty())
            return;
        osl::DirectoryItem item;
        osl::FileStatus status(osl_FileStatus_Mask_FileSize | osl_FileStatus_Mask_ModifyTime);
        sal_uInt64 handleSize = 0;
        if (osl_getFileSize(m_file, &handleSize) != osl_File_E_None || handleSize != m_size
            || osl::DirectoryItem::get(m_url, item) != osl::FileBase::E_None
            || item.getFileStatus(status) != osl::FileBase::E_None
            || status.getFileSize() != m_size
            || status.getModifyTime().Seconds != m_modified.Seconds
            || status.getModifyTime().Nanosec != m_modified.Nanosec)
        {
            m_failed = true;
            m_blocks.clear();
            throw std::runtime_error("The PDF changed; open it again before continuing");
        }
    }
    bool read(sal_uInt64 offset, void* buffer, std::size_t length) noexcept override
    {
        const auto started = std::chrono::steady_clock::now();
        ++m_reads;
        m_bytes += length;
        comphelper::ScopeGuard timing([&] {
            m_readUs += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
        });
        try
        {
            if (*m_stop || offset > m_size || length > m_size - offset)
                return false;
            if (!length)
                return true;
            if (!m_frozen.isEmpty())
            {
                std::memcpy(buffer, m_frozen.getData() + offset, length);
                return true;
            }
            validate();
            auto* out = static_cast<unsigned char*>(buffer);
            while (length)
            {
                if (*m_stop)
                    return false;
                const auto base = offset / Block * Block;
                auto* data = m_blocks.get(base);
                if (!data)
                {
                    ++m_misses;
                    const auto count = std::min<sal_uInt64>(Block, m_size - base);
                    std::vector<unsigned char> bytes(count);
                    sal_uInt64 got = 0;
                    if (osl_readFileAt(m_file, base, bytes.data(), count, &got) != osl_File_E_None
                        || got != count)
                    {
                        m_failed = true;
                        return false;
                    }
                    m_blocks.put(base, std::move(bytes), count);
                    data = m_blocks.get(base);
                }
                const auto within = std::size_t(offset - base);
                const auto count = std::min(length, data->size() - within);
                std::memcpy(out, data->data() + within, count);
                out += count;
                offset += count;
                length -= count;
            }
            validate();
            return true;
        }
        catch (...) { m_failed = true; return false; }
    }
    void freeze(const BinaryDataContainer& bytes) override
    {
        validate();
        m_frozen = bytes;
        m_blocks.clear();
        osl_closeFile(m_file);
        m_file = nullptr;
    }
};
}
std::shared_ptr<Input> openInput(const OUString& url, const std::shared_ptr<std::atomic<bool>>& stop)
{
    auto input = std::make_shared<FileInput>(url, stop);
    input->validate();
    std::vector<unsigned char> head(std::min<sal_uInt64>(1024, input->size()));
    if (!input->read(0, head.data(), head.size()))
        throw std::runtime_error("Cannot read PDF header");
    const unsigned char signature[] = { '%', 'P', 'D', 'F', '-' };
    if (std::search(head.begin(), head.end(), std::begin(signature), std::end(signature)) == head.end())
        throw std::runtime_error("The selected file is not a PDF");
    return input;
}
}
