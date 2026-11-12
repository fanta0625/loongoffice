/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include "reader.hxx"
#include <cppuhelper/bootstrap.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/scopeguard.hxx>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <framework/desktop.hxx>
#include <i18nlangtag/languagetag.hxx>
#include <i18nlangtag/mslangid.hxx>
#include <osl/file.hxx>
#include <osl/process.h>
#include <osl/security.hxx>
#include <rtl/process.h>
#include <rtl/bootstrap.hxx>
#include <sal/main.h>
#include <vcl/svapp.hxx>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace
{
class PdfApplication final : public Application
{
    OUString m_url;
    std::size_t m_budget;
    std::chrono::steady_clock::time_point m_started;
public:
    PdfApplication(OUString url, std::size_t budget, std::chrono::steady_clock::time_point started)
        : m_url(std::move(url)), m_budget(budget), m_started(started) {}
    int Main() override
    {
        auto window = pdffast::createReader(m_url, m_budget, m_started);
        comphelper::ScopeGuard disposeWindow([&] { window.disposeAndClear(); });
        window->Show();
        Execute();
        return 0;
    }
};

OUString fileURL(const OUString& path)
{
    OUString url, cwd, absolute;
    if (path.startsWith("file:"))
        url = path;
    else if (osl::FileBase::getFileURLFromSystemPath(path, url) != osl::FileBase::E_None)
        return {};
    if (osl_getProcessWorkingDir(&cwd.pData) != osl_Process_E_None
        || osl::FileBase::getAbsoluteFileURL(cwd, url, absolute) != osl::FileBase::E_None)
        return {};
    return absolute;
}
}

SAL_IMPLEMENT_MAIN()
{
    const auto started = std::chrono::steady_clock::now();
    OUString input;
    std::size_t cacheMiB = 128;
    bool positional = false;
    for (sal_uInt32 i = 0; i < rtl_getAppCommandArgCount(); ++i)
    {
        OUString arg;
        rtl_getAppCommandArg(i, &arg.pData);
        if (!positional && arg == "--")
            positional = true;
        else if (!positional && (arg == "--help" || arg == "-h"))
        {
            std::cout << "Usage: loongoffice-pdf-fast [--cache-mib=16..4096] [--] [local.pdf]\n"
                         "F2 edits the current page; Esc returns to reading.\n"
                         "Original Office import/conversion routes are unchanged.\n";
            return 0;
        }
        else if (!positional && arg.startsWith("--cache-mib="))
        {
            const auto digits = arg.copy(12);
            const auto value = digits.toInt32();
            bool numeric = !digits.isEmpty();
            for (sal_Int32 index = 0; index < digits.getLength(); ++index)
                numeric = numeric && digits[index] >= '0' && digits[index] <= '9';
            if (!numeric || value < 16 || value > 4096
                || std::size_t(value) > std::numeric_limits<std::size_t>::max() / (1024 * 1024))
            {
                std::cerr << "Cache budget must be between 16 and 4096 MiB\n";
                return 2;
            }
            cacheMiB = value;
        }
        else if ((!positional && arg.startsWith("-")) || !input.isEmpty())
        {
            std::cerr << "Expected one local PDF path; use --help for supported options\n";
            return 2;
        }
        else
            input = arg;
    }
    OUString url;
    if (!input.isEmpty())
    {
        url = fileURL(input);
        if (url.isEmpty())
        {
            std::cerr << "Invalid local file path\n";
            return 2;
        }
    }
    bool initialized = false;
    int result = 1;
    std::unique_ptr<PdfApplication> app;
    try
    {
        app = std::make_unique<PdfApplication>(url, cacheMiB * 1024 * 1024, started);
        OUString fundamental;
        if (!rtl::Bootstrap::get(u"URE_BOOTSTRAP"_ustr, fundamental))
        {
            OUString executable;
            if (osl_getExecutableFile(&executable.pData) != osl_Process_E_None)
                throw std::runtime_error("Cannot locate the PDF reader installation");
            fundamental = executable.copy(0, executable.lastIndexOf('/') + 1)
#ifdef MACOSX
                + u"../Resources/"_ustr
#endif
                + SAL_CONFIGFILE("fundamental");
            rtl::Bootstrap::set(u"URE_BOOTSTRAP"_ustr, rtl::Bootstrap::encode(fundamental));
        }
        // This executable can coexist with Office. Never share its writable
        // configuration directory or depend on Desktop's profile migration.
        OUString profile;
        if (!rtl::Bootstrap::get(u"PdfFastUserInstallation"_ustr, profile))
        {
            osl::Security security;
            if (!security.getConfigDir(profile))
                throw std::runtime_error("Cannot locate the reader configuration directory");
            profile += u"/loongoffice-pdf-fast"_ustr;
        }
        const auto created = osl::Directory::createPath(profile);
        if (created != osl::FileBase::E_None && created != osl::FileBase::E_EXIST)
            throw std::runtime_error("Cannot create the reader configuration directory");
        rtl::Bootstrap::set(u"UserInstallation"_ustr, profile);
        auto context = cppu::defaultBootstrap_InitialComponentContext();
        css::uno::Reference<css::lang::XMultiServiceFactory> manager(
            context->getServiceManager(), css::uno::UNO_QUERY_THROW);
        comphelper::setProcessServiceFactory(manager);
        LanguageTag::setConfiguredSystemLanguage(MsLangId::getSystemLanguage());
        Application::SetAppName(u"LoongOffice PDF Fast"_ustr);
        if (!InitVCL())
            throw std::runtime_error("Cannot initialize the native UI");
        initialized = true;
        result = app->Main();
        framework::getDesktop(comphelper::getProcessComponentContext())->terminate();
        DeInitVCL();
        initialized = false;
        comphelper::setProcessServiceFactory(nullptr);
    }
    catch (const css::uno::Exception& error)
    {
        result = 1;
        std::cerr << "PDF Fast: " << error.Message << '\n';
    }
    catch (const std::exception& error)
    {
        result = 1;
        std::cerr << "PDF Fast: " << error.what() << '\n';
    }
    catch (...)
    {
        result = 1;
        std::cerr << "PDF Fast: unexpected application failure\n";
    }
    if (initialized)
        DeInitVCL();
    comphelper::setProcessServiceFactory(nullptr);
    app.reset();
    return result;
}
