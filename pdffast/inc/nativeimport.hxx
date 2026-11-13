/* This file is part of the LibreOffice project, MPL-2.0. */
#pragma once
#include <sal/types.h>
#include <rtl/ustring.hxx>
#include <memory>
#include <optional>
#include <vector>
#include <chrono>
#include <functional>

namespace pdffast
{
// Pure file/process transport. Never creates a VCL, UNO or Draw object.
// One outstanding result owns the helper's binary spool until release().
class NativeImport final
{
public:
    struct Result
    {
        int page = -1;
        bool success = false;
        OUString commands;
        OUString binary;
        OUString error;
    };
    explicit NativeImport(std::shared_ptr<const std::vector<sal_uInt8>>,
                          std::chrono::milliseconds timeout = std::chrono::minutes(2),
                          std::function<void()> ready = {});
    // ready runs on the transport thread after publication, outside its lock.
    // It must only signal the consumer; destruction joins any active callback.
    ~NativeImport();
    bool request(int page);
    std::optional<Result> takeResult();
    Result wait(); // Only for explicit synchronous operations (save/tests).
    void release();
    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
