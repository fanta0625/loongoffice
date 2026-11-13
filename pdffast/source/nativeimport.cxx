/* This file is part of the LibreOffice project, MPL-2.0. */
#include "nativeimport.hxx"
#include <config_folders.h>
#include <osl/file.h>
#include <osl/process.h>
#include <rtl/bootstrap.hxx>
#include <sal/log.hxx>
#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace pdffast
{
namespace
{
struct File
{
    OUString url;
    oslFileHandle handle = nullptr;
    ~File()
    {
        close();
        if (!url.isEmpty())
            osl_removeFile(url.pData);
    }
    void close()
    {
        if (handle)
            osl_closeFile(handle);
        handle = nullptr;
    }
    void create()
    {
        if (url.isEmpty())
        {
            if (osl_createTempFile(nullptr, &handle, &url.pData) != osl_File_E_None)
                throw std::runtime_error("Cannot create PDF import spool");
        }
        else
        {
            if (osl_openFile(url.pData, &handle, osl_File_OpenFlag_Write) != osl_File_E_None
                || osl_setFileSize(handle, 0) != osl_File_E_None)
                throw std::runtime_error("Cannot reset PDF import spool");
        }
    }
};
void write(oslFileHandle handle, const void* data, sal_uInt64 size)
{
    const auto* bytes = static_cast<const char*>(data);
    while (size)
    {
        sal_uInt64 written = 0;
        if (osl_writeFile(handle, bytes, size, &written) != osl_File_E_None || !written)
            throw std::runtime_error("Cannot write PDF import data");
        bytes += written;
        size -= written;
    }
}
class Lines
{
    oslFileHandle m_handle;
    std::array<char, 65536> m_buffer;
    sal_uInt64 m_pos = 0, m_end = 0;

public:
    explicit Lines(oslFileHandle handle)
        : m_handle(handle)
    {
    }
    std::string next()
    {
        std::string line;
        for (;;)
        {
            if (m_pos == m_end)
            {
                m_pos = 0;
                if (osl_readFile(m_handle, m_buffer.data(), m_buffer.size(), &m_end)
                        != osl_File_E_None
                    || !m_end)
                    throw std::runtime_error("PDF import helper closed its command stream");
            }
            const char c = m_buffer[m_pos++];
            if (c == '\n')
                return line;
            if (c != '\r')
                line += c;
            if (line.size() > 4 * 1024 * 1024)
                throw std::runtime_error("Oversized PDF import command");
        }
    }
};
}

struct NativeImport::Impl
{
    std::shared_ptr<const std::vector<sal_uInt8>> source;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping = false, active = false, leased = false, cancelled = false;
    bool snapshotReady = false;
    const std::chrono::milliseconds timeout;
    const std::function<void()> ready;
    std::chrono::steady_clock::time_point deadline;
    bool timedOut = false;
    std::optional<int> pending;
    std::optional<Result> result;
    oslProcess process = nullptr; // protected by mutex, including termination
    oslFileHandle input = nullptr, output = nullptr, errors = nullptr;
    std::unique_ptr<Lines> lines;
    std::thread drain;
    File snapshot, commands, binary;
    std::thread watchdog;
    std::thread worker;

    Impl(std::shared_ptr<const std::vector<sal_uInt8>> bytes, std::chrono::milliseconds limit,
         std::function<void()> notify)
        : source(std::move(bytes))
        , timeout(std::max(limit, std::chrono::milliseconds(0)))
        , ready(std::move(notify))
        , watchdog([this] { watch(); })
        , worker([this] { run(); })
    {
    }
    ~Impl()
    {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
            if (process)
                osl_terminateProcess(process);
        }
        condition.notify_all();
        worker.join();
        watchdog.join();
    }
    void expireLocked()
    {
        if (active && !cancelled && std::chrono::steady_clock::now() >= deadline)
        {
            timedOut = cancelled = true;
            if (process)
                osl_terminateProcess(process);
        }
    }
    void watch()
    {
        std::unique_lock lock(mutex);
        while (!stopping)
        {
            if (!active || cancelled)
                condition.wait(lock, [&] { return stopping || (active && !cancelled); });
            else
            {
                const auto limit = deadline;
                condition.wait_until(lock, limit, [&] {
                    return stopping || !active || cancelled || deadline != limit;
                });
                expireLocked();
            }
        }
    }
    void checkStop()
    {
        std::scoped_lock lock(mutex);
        expireLocked();
        if (timedOut)
            throw std::runtime_error("PDF import timed out");
        if (stopping || cancelled)
            throw std::runtime_error("PDF import cancelled");
    }
    void closeHelper()
    {
        oslProcess old;
        {
            std::scoped_lock lock(mutex);
            old = process;
            if (old)
                osl_terminateProcess(old);
            process = nullptr;
        }
        if (input)
            osl_closeFile(input);
        if (old)
            osl_joinProcess(old);
        if (drain.joinable())
            drain.join();
        if (output)
            osl_closeFile(output);
        if (errors)
            osl_closeFile(errors);
        if (old)
            osl_freeProcessHandle(old);
        input = output = errors = nullptr;
        lines.reset();
    }
    void openHelper()
    {
        if (process)
            return;
        checkStop();
        if (!snapshotReady)
        {
            if (!source || source->empty())
                throw std::runtime_error("Missing frozen PDF source");
            snapshot.create();
            for (size_t offset = 0; offset < source->size();)
            {
                checkStop();
                const size_t count = std::min<size_t>(65536, source->size() - offset);
                write(snapshot.handle, source->data() + offset, count);
                offset += count;
            }
            snapshot.close();
            snapshotReady = true;
            SAL_INFO("pdffast", "native-source-snapshot bytes=" << source->size());
        }
        if (binary.url.isEmpty())
        {
            binary.create();
            binary.close();
        }
        OUString sourcePath, binaryPath;
        if (osl_getSystemPathFromFileURL(snapshot.url.pData, &sourcePath.pData) != osl_File_E_None
            || osl_getSystemPathFromFileURL(binary.url.pData, &binaryPath.pData) != osl_File_E_None)
            throw std::runtime_error("Invalid PDF import spool path");
        OUString executable(u"$BRAND_BASE_DIR/" LIBO_BIN_FOLDER "/xpdfimport"_ustr);
        rtl::Bootstrap::expandMacros(executable);
        OUString fileOption(u"-f"_ustr), option(u"-o"_ustr),
            session(u"LoongOfficePdfSession=1"_ustr);
        rtl_uString* args[]
            = { sourcePath.pData, fileOption.pData, binaryPath.pData, option.pData, session.pData };
        {
            std::scoped_lock lock(mutex);
            if (stopping || cancelled)
                throw std::runtime_error("PDF import cancelled");
            const auto security = osl_getCurrentSecurity();
            const auto error = osl_executeProcess_WithRedirectedIO(
                executable.pData, args, 5, osl_Process_SEARCHPATH | osl_Process_HIDDEN, security,
                nullptr, nullptr, 0, &process, &input, &output, &errors);
            osl_freeSecurityHandle(security);
            if (error != osl_Process_E_None || !input || !output || !errors)
                throw std::runtime_error("Cannot start PDF import helper");
        }
        // Binary resources go to -f, but drain diagnostic stderr independently
        // so a noisy/crashing helper cannot deadlock against a full pipe.
        drain = std::thread([this] {
            std::array<char, 4096> buffer;
            sal_uInt64 count = 0;
            while (osl_readFile(errors, buffer.data(), buffer.size(), &count) == osl_File_E_None
                   && count)
            {
            }
        });
        lines = std::make_unique<Lines>(output);
        write(input, "P\nO\n", 4);
        if (lines->next() != "#OPEN")
            throw std::runtime_error("Cannot open PDF in native import helper");
        SAL_INFO("pdffast", "native-edit-session opened");
    }
    void run()
    {
        for (;;)
        {
            int page;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || (pending && !leased); });
                if (stopping)
                    break;
                page = *pending;
                pending.reset();
            }
            Result value;
            value.page = page;
            const auto started = std::chrono::steady_clock::now();
            try
            {
                openHelper();
                checkStop();
                commands.create();
                const auto request = "R" + std::to_string(page + 1) + "\n";
                write(input, request.data(), request.size());
                for (;;)
                {
                    auto line = lines->next();
                    if (line == "#DONE")
                        break;
                    if (line.starts_with("#"))
                        throw std::runtime_error("Native PDF page conversion failed");
                    line += '\n';
                    write(commands.handle, line.data(), line.size());
                }
                commands.close();
                value.success = true;
                value.commands = commands.url;
                value.binary = binary.url;
            }
            catch (const std::exception& error)
            {
                snapshot.close();
                commands.close();
                value.error = OUString::fromUtf8(error.what());
                closeHelper();
            }
            bool wasCancelled;
            bool wasTimedOut;
            {
                std::scoped_lock lock(mutex);
                expireLocked();
                active = false;
                leased = true;
                wasCancelled = cancelled;
                wasTimedOut = timedOut;
            }
            if (wasCancelled)
            {
                // cancel() can race the final #DONE. Never reuse a process
                // which was terminated just as its result was being published.
                closeHelper();
                value.success = false;
                value.error
                    = wasTimedOut ? u"PDF import timed out"_ustr : u"PDF import cancelled"_ustr;
            }
            SAL_INFO("pdffast", "native-edit-transport page="
                                    << page << " success=" << value.success << " total_us="
                                    << std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count());
            {
                std::scoped_lock lock(mutex);
                result = std::move(value);
            }
            condition.notify_all();
            if (ready)
                ready();
        }
        closeHelper();
    }
};

NativeImport::NativeImport(std::shared_ptr<const std::vector<sal_uInt8>> bytes,
                           std::chrono::milliseconds timeout, std::function<void()> ready)
    : m_impl(std::make_unique<Impl>(std::move(bytes), timeout, std::move(ready)))
{
}
NativeImport::~NativeImport() = default;
bool NativeImport::request(int page)
{
    std::scoped_lock lock(m_impl->mutex);
    if (page < 0 || page == std::numeric_limits<int>::max() || m_impl->active || m_impl->leased
        || m_impl->stopping)
        return false;
    m_impl->active = true;
    m_impl->cancelled = false;
    m_impl->timedOut = false;
    m_impl->deadline = std::chrono::steady_clock::now() + m_impl->timeout;
    m_impl->pending = page;
    m_impl->condition.notify_all();
    return true;
}
std::optional<NativeImport::Result> NativeImport::takeResult()
{
    std::scoped_lock lock(m_impl->mutex);
    auto result = std::move(m_impl->result);
    m_impl->result.reset();
    return result;
}
NativeImport::Result NativeImport::wait()
{
    std::unique_lock lock(m_impl->mutex);
    m_impl->condition.wait(lock, [&] { return m_impl->result.has_value(); });
    auto result = std::move(*m_impl->result);
    m_impl->result.reset();
    return result;
}
void NativeImport::release()
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->result.reset();
    m_impl->leased = false;
    m_impl->condition.notify_all();
}
void NativeImport::cancel()
{
    std::scoped_lock lock(m_impl->mutex);
    m_impl->cancelled = true;
    if (m_impl->active && m_impl->process)
        osl_terminateProcess(m_impl->process);
}
}
