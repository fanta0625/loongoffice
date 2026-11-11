// SPDX-License-Identifier: MPL-2.0
// Page-level orchestration only. PDF conversion and Draw XML import stay in
// their existing services; no replacement parser or object-copy loop lives here.
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/drawing/XDrawView.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/uno/RuntimeException.hpp>
#include <com/sun/star/document/XFilter.hpp>
#include <com/sun/star/document/XImporter.hpp>
#include <com/sun/star/drawing/XDrawPage.hpp>
#include <com/sun/star/drawing/XShape.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/io/XInputStream.hpp>
#include <com/sun/star/lang/IllegalArgumentException.hpp>
#include <com/sun/star/lang/XServiceInfo.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/ucb/SimpleFileAccess.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/xml/XImportFilter.hpp>
#include <com/sun/star/xml/sax/XDocumentHandler.hpp>
#include <comphelper/propertyvalue.hxx>
#include <comphelper/diagnose_ex.hxx>
#include <comphelper/scopeguard.hxx>
#include <cppuhelper/implbase.hxx>
#include <cppuhelper/supportsservice.hxx>
#include <sal/log.hxx>
#include <vcl/svapp.hxx>
#include <atomic>
#include <utility>

using namespace com::sun::star;

namespace
{
class PDFPageImportFilter final
    : public cppu::WeakImplHelper<document::XFilter, document::XImporter, lang::XServiceInfo>
{
    uno::Reference<uno::XComponentContext> mxContext;
    uno::Reference<lang::XComponent> mxDocument;
    uno::Reference<drawing::XDrawPage> mxPage;
    uno::Sequence<uno::Any> maArguments;
    std::atomic<bool> mbCancelled{ false };

public:
    PDFPageImportFilter(uno::Reference<uno::XComponentContext> xContext,
                        const uno::Sequence<uno::Any>& rArguments)
        : mxContext(std::move(xContext))
        , maArguments(rArguments)
    {
        for (const auto& rArgument : maArguments)
        {
            beans::PropertyValue aProperty;
            if ((rArgument >>= aProperty) && aProperty.Name == "TargetPage")
                aProperty.Value >>= mxPage;
        }
    }

    void SAL_CALL setTargetDocument(const uno::Reference<lang::XComponent>& xDocument) override
    {
        SolarMutexGuard aGuard;
        mxDocument = xDocument;
    }

    sal_Bool SAL_CALL filter(const uno::Sequence<beans::PropertyValue>& rDescriptor) override
    {
        // Match the outer execution boundary of SfxBaseModel::load's
        // SfxModelGuard. A model's lockControllers() is a different, boolean
        // notification lock: it does not keep the GUI from painting partially
        // imported objects between the individual shape UNO methods.
        // The guard must live on this call's stack, never across UNO calls.
        SolarMutexGuard aGuard;
        OUString aURL;
        sal_Int32 nPage = 0;
        bool bAutomatic = false;
        for (const auto& rProperty : rDescriptor)
        {
            if (rProperty.Name == "URL")
                rProperty.Value >>= aURL;
            else if (rProperty.Name == "SourcePage")
                rProperty.Value >>= nPage;
            else if (rProperty.Name == "Automatic")
                rProperty.Value >>= bAutomatic;
        }
        if (!mxDocument.is() || !mxPage.is() || aURL.isEmpty() || nPage < 1)
            throw lang::IllegalArgumentException(u"PDF page import needs target, URL and SourcePage"_ustr,
                                                  {}, 0);
        if (mbCancelled)
            return false;

        uno::Reference<frame::XModel> xModel(mxDocument, uno::UNO_QUERY_THROW);
        if (bAutomatic)
        {
            auto xController = xModel->getCurrentController();
            uno::Reference<drawing::XDrawView> xView(xController, uno::UNO_QUERY_THROW);
            uno::Reference<beans::XPropertySet> xProperties(xController, uno::UNO_QUERY_THROW);
            OUString aBusyReason;
            if (xView->getCurrentPage() != mxPage
                || !(xProperties->getPropertyValue(u"PdfReaderEditBusyReason"_ustr) >>= aBusyReason)
                || !aBusyReason.isEmpty())
                throw uno::RuntimeException(u"PDFReaderAutomaticEditDeferred"_ustr, {});
        }

        // Construct the existing content-only importer and let it validate
        // target membership, preview count, layer and resource prefix first.
        auto xServices = mxContext->getServiceManager();
        uno::Reference<xml::sax::XDocumentHandler> xHandler(
            xServices->createInstanceWithArgumentsAndContext(
                u"org.loongoffice.comp.Draw.PDFPageContentImporter"_ustr, maArguments, mxContext),
            uno::UNO_QUERY_THROW);
        uno::Reference<document::XImporter>(xHandler, uno::UNO_QUERY_THROW)
            ->setTargetDocument(mxDocument);
        uno::Reference<drawing::XShape> xPreview(mxPage->getByIndex(0), uno::UNO_QUERY_THROW);
        const bool bOwnModelLock = !xModel->hasControllersLocked();
        if (bOwnModelLock)
            xModel->lockControllers();
        comphelper::ScopeGuard aModelUnlock([&] {
            if (bOwnModelLock)
                xModel->unlockControllers();
        });

        // Remove only this request's objects on failure, before releasing the
        // execution guard. Python retains resource/style cleanup and the
        // navigation/undo/preview commit transaction, including injected failures.
        bool bSuccess = false;
        comphelper::ScopeGuard aRollback([&] {
            if (bSuccess)
                return;
            try
            {
                for (sal_Int32 i = mxPage->getCount(); i-- > 0;)
                {
                    uno::Reference<drawing::XShape> xShape(mxPage->getByIndex(i), uno::UNO_QUERY_THROW);
                    if (xShape != xPreview)
                        mxPage->remove(xShape);
                }
            }
            catch (const uno::Exception&)
            {
                TOOLS_WARN_EXCEPTION("xmloff.draw", "PDF page rollback; session will retry cleanup");
            }
        });

        auto xFiles = ucb::SimpleFileAccess::create(mxContext);
        uno::Reference<io::XInputStream> xStream = xFiles->openFileRead(aURL);
        comphelper::ScopeGuard aClose([&] {
            try
            {
                xStream->closeInput(); // The raw converter normally closes it too.
            }
            catch (const uno::Exception&)
            {
            }
        });
        uno::Reference<xml::XImportFilter> xConverter(
            xServices->createInstanceWithContext(u"org.libreoffice.comp.documents.DrawPDFImport"_ustr,
                                                 mxContext),
            uno::UNO_QUERY_THROW);
        const OUString aOptions = u"Page="_ustr + OUString::number(nPage);
        bSuccess = xConverter->importer(
            { comphelper::makePropertyValue(u"URL"_ustr, aURL),
              comphelper::makePropertyValue(u"InputStream"_ustr, xStream),
              comphelper::makePropertyValue(u"FilterOptions"_ustr, aOptions) },
            xHandler, {});
        bSuccess = bSuccess && !mbCancelled;
        return bSuccess;
    }

    void SAL_CALL cancel() override
    {
        // Synchronous conversion cannot be interrupted mid-parser. A concurrent
        // cancel requests rollback after the converter returns, not preemption.
        mbCancelled = true;
    }
    OUString SAL_CALL getImplementationName() override { return u"PDFPageImportFilter"_ustr; }
    sal_Bool SAL_CALL supportsService(const OUString& rName) override
    {
        return cppu::supportsService(this, rName);
    }
    uno::Sequence<OUString> SAL_CALL getSupportedServiceNames() override
    {
        return { u"org.loongoffice.comp.Draw.PDFPageImportFilter"_ustr };
    }
};
}

extern "C" SAL_DLLPUBLIC_EXPORT uno::XInterface*
org_loongoffice_comp_Draw_PDFPageImportFilter_get_implementation(
    uno::XComponentContext* pContext, uno::Sequence<uno::Any> const& rArguments)
{
    return cppu::acquire(new PDFPageImportFilter(pContext, rArguments));
}
