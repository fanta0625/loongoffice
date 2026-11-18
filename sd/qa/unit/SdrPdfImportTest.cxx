/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <test/unoapi_test.hxx>
#include <memory>
#include <optional>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <config_features.h>
#include <com/sun/star/drawing/XDrawPageDuplicator.hpp>
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
#include <PdfFastDocument.hxx>
#include <drawdoc.hxx>
#include <svx/svdview.hxx>
#include <svx/svdpagv.hxx>
#include <svx/svdundo.hxx>
#include <svx/sdr/contact/objectcontact.hxx>
#include <svx/sdr/contact/viewcontact.hxx>
#include <svx/sdr/contact/viewobjectcontact.hxx>
#include <svl/undo.hxx>
#endif

#include <comphelper/scopeguard.hxx>
#include <comphelper/propertysequence.hxx>
#include <comphelper/propertyvalue.hxx>

#include <unotools/tempfile.hxx>
#include <unotools/mediadescriptor.hxx>
#include <svx/svdograf.hxx>
#include <editeng/outlobj.hxx>
#include <editeng/editobj.hxx>
#include <vcl/filter/PDFiumLibrary.hxx>
#include <vcl/pdf/PDFPageObjectType.hxx>
#include <vcl/vectorgraphicdata.hxx>
#include <vcl/pdf/PDFAnnotationSubType.hxx>
#include <vcl/bitmap.hxx>
#include <vcl/gfxlink.hxx>
#include <vcl/filter/PngImageWriter.hxx>
#include <vcl/svapp.hxx>
#include <vcl/pdfread.hxx>
#include <vcl/virdev.hxx>
#include <tools/stream.hxx>

#include <Annotation.hxx>
#include <DrawDocShell.hxx>
#include <ViewShell.hxx>
#include <sdpage.hxx>
#include <unomodel.hxx>

using namespace css;

namespace
{
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
class CountingViewObjectContact final : public sdr::contact::ViewObjectContact
{
public:
    using ViewObjectContact::ViewObjectContact;
    unsigned changes = 0;

    void ActionChanged() override
    {
        ++changes;
        ViewObjectContact::ActionChanged();
    }
};

void dumpPdfBitmap(const OUString& name, const Bitmap& bitmap)
{
    if (const char* directory = getenv("LO_PDFFAST_TEST_DUMP"))
    {
        SvFileStream stream(OUString::fromUtf8(directory) + u"/"_ustr + name + u".png"_ustr,
                            StreamMode::WRITE | StreamMode::TRUNC);
        vcl::PngImageWriter(stream).write(Graphic(bitmap));
    }
}

#endif

class EnvVarGuard
{
public:
    EnvVarGuard(const char* var, const char* val)
    {
        sVar = var;
        if (const char* previous = getenv(var))
            m_previous = previous;
        SetEnv(var, val);
    }
    ~EnvVarGuard() { SetEnv(sVar, m_previous ? m_previous->c_str() : nullptr); }

private:
    static void SetEnv(const char* var, const char* val)
    {
#ifdef _WIN32
        if (!val)
            val = ""; // remove
        _putenv_s(var, val);
#else
        if (val)
            setenv(var, val, true);
        else
            unsetenv(var);
#endif
    }

    const char* sVar = nullptr;
    std::optional<std::string> m_previous;
};
}

class SdrPdfImportTest : public UnoApiTest
{
public:
    SdrPdfImportTest()
        : UnoApiTest(u"/sd/qa/unit/data/"_ustr)
    {
    }
};

// Load the PDF in Draw, which will load the PDF as an Graphic, then
// mark the graphic object and trigger "break" function. This should
// convert the PDF content into objects/shapes.
CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testImportSimpleText)
{
    auto pPdfium = vcl::pdf::PDFiumLibrary::get();
    if (!pPdfium)
    {
        return;
    }

    // We need to enable PDFium import (and make sure to disable after the test)
    EnvVarGuard UsePDFiumGuard("LO_IMPORT_USE_PDFIUM", "1");

    loadFromFile(u"SimplePDF.pdf");
    auto pImpressDocument = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    sd::ViewShell* pViewShell = pImpressDocument->GetDocShell()->GetViewShell();
    CPPUNIT_ASSERT(pViewShell);

    // Get the first page - there should be only one.
    SdPage* pPage = pViewShell->GetActualPage();
    CPPUNIT_ASSERT(pPage);

    // Check there is one object on the page only
    CPPUNIT_ASSERT_EQUAL(size_t(1), pPage->GetObjCount());

    // Get the first object - there should be only one.
    SdrObject* pObject = pPage->GetObj(0);
    CPPUNIT_ASSERT(pObject);

    // Check the object is a graphic object
    SdrGrafObj* pGraphicObject = dynamic_cast<SdrGrafObj*>(pObject);
    CPPUNIT_ASSERT(pGraphicObject);
    // Check the graphic is a vector graphic and that it is PDF
    Graphic aGraphic = pGraphicObject->GetGraphic();
    auto const& pVectorGraphicData = aGraphic.getVectorGraphicData();
    CPPUNIT_ASSERT(pVectorGraphicData);
    CPPUNIT_ASSERT_EQUAL(VectorGraphicDataType::Pdf, pVectorGraphicData->getType());

    // Mark the object
    SdrView* pView = pViewShell->GetView();
    pView->MarkObj(pObject, pView->GetSdrPageView());

    // Execute the break operation - to turn the PDF into shapes/objects
    pViewShell->GetDrawView()->DoImportMarkedMtf();

    // Check there is one object on the page only
    CPPUNIT_ASSERT_EQUAL(size_t(1), pPage->GetObjCount());

    // Get the object
    SdrObject* pImportedObject = pPage->GetObj(0);
    CPPUNIT_ASSERT(pImportedObject);

    // Check the object position
#if !defined _WIN32
    CPPUNIT_ASSERT_EQUAL(Point(2004, 2018), pImportedObject->GetLogicRect().GetPos());
#else
    // need to check why windows appears to be different
    CPPUNIT_ASSERT_EQUAL(Point(1998, 2018), pImportedObject->GetLogicRect().GetPos());
#endif

    // Check the object size
#if !defined _WIN32
    CPPUNIT_ASSERT_EQUAL(Size(2165, 470), pImportedObject->GetLogicRect().GetSize());
#else
    // need to check why windows appears to be different
    CPPUNIT_ASSERT_EQUAL(Size(3944, 470), pImportedObject->GetLogicRect().GetSize());
#endif

    // Object should be a text object containing one paragraph with
    // content "This is PDF!"

    SdrTextObj* pTextObject = DynCastSdrTextObj(pImportedObject);
    CPPUNIT_ASSERT(pTextObject);
    OutlinerParaObject* pOutlinerParagraphObject = pTextObject->GetOutlinerParaObject();
    const EditTextObject& aEdit = pOutlinerParagraphObject->GetTextObject();
    OUString sText = aEdit.GetText(0);
    CPPUNIT_ASSERT_EQUAL(u"This is PDF!"_ustr, sText);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testAnnotationsImportExport)
{
    auto pPdfium = vcl::pdf::PDFiumLibrary::get();
    if (!pPdfium)
    {
        return;
    }

    // We need to enable PDFium import (and make sure to disable after the test)
    EnvVarGuard UsePDFiumGuard("LO_IMPORT_USE_PDFIUM", "1");

    EnvVarGuard DisablePDFCompressionGuard("VCL_DEBUG_DISABLE_PDFCOMPRESSION", "1");

    auto pPdfiumLibrary = vcl::pdf::PDFiumLibrary::get();

    loadFromFile(u"PdfWithAnnotation.pdf");
    auto pImpressDocument = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    sd::ViewShell* pViewShell = pImpressDocument->GetDocShell()->GetViewShell();
    CPPUNIT_ASSERT(pViewShell);

    BinaryDataContainer aContainer;

    {
        // Get the first page - there should be only one.
        SdPage* pPage = pViewShell->GetActualPage();
        CPPUNIT_ASSERT(pPage);

        // Check the number of annotations
        CPPUNIT_ASSERT_EQUAL(size_t(1), pPage->getAnnotations().size());

        // Get the first object - there should be only one.
        SdrObject* pObject = pPage->GetObj(0);
        CPPUNIT_ASSERT(pObject);

        // Check the object is a graphic object
        SdrGrafObj* pGraphicObject = dynamic_cast<SdrGrafObj*>(pObject);
        CPPUNIT_ASSERT(pGraphicObject);

        // Check the graphic is a vector graphic and that it is PDF
        Graphic aGraphic = pGraphicObject->GetGraphic();
        auto const& pVectorGraphicData = aGraphic.getVectorGraphicData();
        CPPUNIT_ASSERT(pVectorGraphicData);
        CPPUNIT_ASSERT_EQUAL(VectorGraphicDataType::Pdf, pVectorGraphicData->getType());

        // Write the PDF
        aContainer = pVectorGraphicData->getBinaryDataContainer();
    }

    { // check graphic PDF has annotations

        CPPUNIT_ASSERT_EQUAL(false, aContainer.isEmpty());

        auto pPDFDocument
            = pPdfiumLibrary->openDocument(aContainer.getData(), aContainer.getSize(), OString());
        auto pPDFPage = pPDFDocument->openPage(0);

        CPPUNIT_ASSERT_EQUAL(2, pPDFPage->getAnnotationCount());

        auto pPDFAnnotation1 = pPDFPage->getAnnotation(0);
        CPPUNIT_ASSERT_EQUAL(vcl::pdf::PDFAnnotationSubType::Text,
                             pPDFAnnotation1->getSubType()); // Text annotation

        auto pPDFAnnotation2 = pPDFPage->getAnnotation(1);
        CPPUNIT_ASSERT_EQUAL(vcl::pdf::PDFAnnotationSubType::Popup,
                             pPDFAnnotation2->getSubType()); // Pop-up annotation
    }

    { // save as PDF and check annotations
        uno::Reference<frame::XStorable> xStorable(mxComponent, uno::UNO_QUERY);
        utl::MediaDescriptor aMediaDescriptor;
        aMediaDescriptor[u"FilterName"_ustr] <<= u"writer_pdf_Export"_ustr;
        uno::Sequence<beans::PropertyValue> aFilterData(
            comphelper::InitPropertySequence({ { "ExportBookmarks", uno::Any(true) } }));
        aMediaDescriptor[u"FilterData"_ustr] <<= aFilterData;
        xStorable->storeToURL(maTempFile.GetURL(), aMediaDescriptor.getAsConstPropertyValueList());

        // Check PDF for annotations
        auto pPDFDocument = parsePDFExport();
        CPPUNIT_ASSERT(pPDFDocument);
        CPPUNIT_ASSERT_EQUAL(1, pPDFDocument->getPageCount());

        auto pPDFPage = pPDFDocument->openPage(0);
        CPPUNIT_ASSERT(pPDFPage);

        CPPUNIT_ASSERT_EQUAL(2, pPDFPage->getAnnotationCount());

        auto pPDFAnnotation1 = pPDFPage->getAnnotation(0);
        CPPUNIT_ASSERT_EQUAL(vcl::pdf::PDFAnnotationSubType::Text,
                             pPDFAnnotation1->getSubType()); // Text annotation

        auto pPDFAnnotation2 = pPDFPage->getAnnotation(1);
        CPPUNIT_ASSERT_EQUAL(vcl::pdf::PDFAnnotationSubType::Popup,
                             pPDFAnnotation2->getSubType()); // Pop-up annotation

        // Load document again
        loadFromURL(maTempFile.GetURL());
        auto pNewImpressDocument = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
        sd::ViewShell* pNewViewShell = pNewImpressDocument->GetDocShell()->GetViewShell();
        CPPUNIT_ASSERT(pNewViewShell);

        SdPage* pPage = pNewViewShell->GetActualPage();
        CPPUNIT_ASSERT(pPage);

        // We expect only 1 annotation in the document because the PDF
        // annotations are dependent on each-other:
        // parent annotation "Text" and the child annotation "Pop-up"

        CPPUNIT_ASSERT_EQUAL(size_t(1), pPage->getAnnotations().size());

        // check annotation
        auto xAnnotation = pPage->getAnnotations().at(0);

        CPPUNIT_ASSERT_DOUBLES_EQUAL(90.33, xAnnotation->getPosition().X, 1E-3);
        CPPUNIT_ASSERT_DOUBLES_EQUAL(12.07, xAnnotation->getPosition().Y, 1E-3);

        CPPUNIT_ASSERT_EQUAL(u"TheAuthor"_ustr, xAnnotation->getAuthor());
        CPPUNIT_ASSERT_EQUAL(OUString(), xAnnotation->getInitials());

        auto xText = xAnnotation->getTextRange();

        CPPUNIT_ASSERT_EQUAL(u"This is the annotation text!"_ustr, xText->getString());

        auto aDateTime = xAnnotation->getDateTime();
        CPPUNIT_ASSERT_EQUAL(sal_Int16(2020), aDateTime.Year);
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(6), aDateTime.Month);
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(18), aDateTime.Day);
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(12), aDateTime.Hours);
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(11), aDateTime.Minutes);
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(53), aDateTime.Seconds);
        CPPUNIT_ASSERT_EQUAL(sal_uInt32(0), aDateTime.NanoSeconds);
        CPPUNIT_ASSERT_EQUAL(false, bool(aDateTime.IsUTC));
    }
}

#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testDeferredMetadataSurvivesCloneAndExportPreparation)
{
    if (!vcl::pdf::PDFiumLibrary::get())
        return;
    const auto url = m_directories.getURLFromSrc(u"/sd/qa/unit/data/PdfWithAnnotation.pdf");
    std::vector<vcl::PDFGraphicResult> eager;
    SvFileStream eagerInput(url, StreamMode::READ);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), vcl::ImportPDFUnloaded(eagerInput, eager));
    CPPUNIT_ASSERT(!eager.front().GetAnnotations().empty());
    std::vector<vcl::PDFGraphicResult> deferred;
    std::unique_ptr<vcl::PDFMetadataReader> reader;
    SvFileStream deferredInput(url, StreamMode::READ);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), vcl::ImportPDFUnloaded(deferredInput, deferred, &reader));
    CPPUNIT_ASSERT(reader);
    CPPUNIT_ASSERT(deferred.front().GetAnnotations().empty());
    CPPUNIT_ASSERT(deferred.front().GetLinksInfo().empty());
    CPPUNIT_ASSERT_EQUAL(eager.front().GetSize(), deferred.front().GetSize());
    CPPUNIT_ASSERT(!reader->ReadPage(-1));
    CPPUNIT_ASSERT(!reader->ReadPage(1));
    const auto metadata = reader->ReadPage(0);
    CPPUNIT_ASSERT(metadata);
    CPPUNIT_ASSERT_EQUAL(eager.front().GetAnnotations().size(), metadata->GetAnnotations().size());
    CPPUNIT_ASSERT_EQUAL(eager.front().GetAnnotations().front().maText,
                         metadata->GetAnnotations().front().maText);
    CPPUNIT_ASSERT_EQUAL(eager.front().GetLinksInfo().size(), metadata->GetLinksInfo().size());

    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"PdfWithAnnotation.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* shell = model->GetDocShell();
    auto* doc = shell->GetDoc();
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    // Headless import is intentionally eager. Recreate the desktop deferred
    // boundary to exercise cloning before the metadata idle has run.
    const auto annotations = page->getAnnotations();
    shell->SetModified(false);
    const auto beforeEagerClone = doc->GetSdrUndoManager()->GetUndoActionCount();
    auto eagerClone = page->CloneSdrPage(*doc);
    const bool cloneMarksModified = shell->IsModified();
    const auto cloneUndoDelta = doc->GetSdrUndoManager()->GetUndoActionCount() - beforeEagerClone;
    eagerClone.clear();
    for (const auto& annotation : annotations)
        page->removeAnnotationNoNotify(annotation);
    page->setLinkAnnotations({});
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    controller->SetMetadataReader(std::move(reader));
    auto* original = dynamic_cast<SdrGrafObj*>(page->GetObj(0));
    CPPUNIT_ASSERT(original);
    const Graphic graphic = original->GetGraphic();
    original->SetGraphic(Graphic(graphic.GetSharedGfxLink(), 999));
    CPPUNIT_ASSERT(!controller->EnsureAllMetadata());
    CPPUNIT_ASSERT(page->getAnnotations().empty());
    original->SetGraphic(graphic);
    shell->SetModified(false);
    const auto undoCount = doc->GetSdrUndoManager()->GetUndoActionCount();
    auto clone = page->CloneSdrPage(*doc);
    CPPUNIT_ASSERT(clone.is());
    CPPUNIT_ASSERT_EQUAL(annotations.size(), page->getAnnotations().size());
    CPPUNIT_ASSERT_EQUAL(annotations.size(), clone->getAnnotations().size());
    // Compare with the ordinary eager clone, including native side effects.
    CPPUNIT_ASSERT_EQUAL(cloneMarksModified, shell->IsModified());
    CPPUNIT_ASSERT_EQUAL(undoCount + cloneUndoDelta,
                         doc->GetSdrUndoManager()->GetUndoActionCount());
    shell->SetModified(false);
    const auto afterClone = doc->GetSdrUndoManager()->GetUndoActionCount();
    CPPUNIT_ASSERT(controller->EnsureAllMetadata());
    CPPUNIT_ASSERT(controller->EnsureAllMetadata());
    CPPUNIT_ASSERT_EQUAL(annotations.size(), page->getAnnotations().size());
    CPPUNIT_ASSERT(!shell->IsModified());
    CPPUNIT_ASSERT_EQUAL(afterClone, doc->GetSdrUndoManager()->GetUndoActionCount());
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testMaterializedPageUsesLiveDrawPaint)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* shell = model->GetDocShell();
    auto* doc = shell->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* fast = doc->GetPdfFastDocument();
    SdrView view(*doc);
    comphelper::ScopeGuard forget([&] { fast->ForgetView(view); });
    ScopedVclPtrInstance<VirtualDevice> output;
    output->SetOutputSizePixel(Size(800, 600));
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    view.ShowSdrPage(page);
    CPPUNIT_ASSERT(fast->PreparePaint(view, *output));
    fast->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(page->GetObjCount() > 1);
    // Both editable and returned-to-read-only materialized pages must bypass
    // PDF redirection entirely, at every zoom and on subsequent visits.
    for (const bool readOnly : { false, true })
    {
        shell->SetReadOnlyUI(readOnly);
        for (const int zoom : { 75, 100, 200, 50 })
        {
            MapMode mode(output->GetMapMode());
            mode.SetScaleX(Fraction(zoom, 100));
            mode.SetScaleY(Fraction(zoom, 100));
            output->SetMapMode(mode);
            CPPUNIT_ASSERT(!fast->PreparePaint(view, *output));
        }
    }
    view.HideSdrPage();
    view.ShowSdrPage(doc->GetSdPage(1, PageKind::Standard));
    CPPUNIT_ASSERT(fast->PreparePaint(view, *output));
    view.HideSdrPage();
    view.ShowSdrPage(page);
    CPPUNIT_ASSERT(!fast->PreparePaint(view, *output));
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testVisiblePdfThumbnailsArriveWithoutModelChanges)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* fast = doc->GetPdfFastDocument();
    const auto count = doc->GetSdPageCount(PageKind::Standard);
    model->GetDocShell()->SetModified(false);
    const auto undoCount = doc->GetSdrUndoManager()->GetUndoActionCount();
    std::vector<std::vector<rtl::Reference<SdrObject>>> originals;
    sdr::contact::ObjectContact objectContact;
    std::vector<std::unique_ptr<CountingViewObjectContact>> contacts;
    for (sal_uInt16 n = 0; n < count; ++n)
    {
        auto* page = doc->GetSdPage(n, PageKind::Standard);
        CPPUNIT_ASSERT(page);
        CPPUNIT_ASSERT(page->GetObjCount() > 0);
        originals.emplace_back();
        contacts.push_back(
            std::make_unique<CountingViewObjectContact>(objectContact, page->GetViewContact()));
        for (size_t index = 0; index < page->GetObjCount(); ++index)
        {
            auto* object = page->GetObj(index);
            CPPUNIT_ASSERT(object);
            originals.back().emplace_back(object);
            contacts.push_back(std::make_unique<CountingViewObjectContact>(
                objectContact, object->GetViewContact()));
        }
        Bitmap bitmap;
        fast->GetPagePreview(*page, Size(160, 200), bitmap);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool ready = false;
    while (!ready && std::chrono::steady_clock::now() < deadline)
    {
        Application::Reschedule();
        ready = true;
        for (sal_uInt16 n = 0; n < count; ++n)
        {
            Bitmap bitmap;
            const auto result = fast->GetPagePreview(*doc->GetSdPage(n, PageKind::Standard),
                                                     Size(160, 200), bitmap);
            ready &= result == sd::PdfFastDocument::PreviewResult::Ready;
            if (result == sd::PdfFastDocument::PreviewResult::Ready)
                CPPUNIT_ASSERT(!bitmap.IsEmpty());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CPPUNIT_ASSERT(ready);
    CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
    CPPUNIT_ASSERT_EQUAL(undoCount, doc->GetSdrUndoManager()->GetUndoActionCount());
    for (sal_uInt16 n = 0; n < count; ++n)
    {
        auto* page = doc->GetSdPage(n, PageKind::Standard);
        CPPUNIT_ASSERT_EQUAL(originals[n].size(), page->GetObjCount());
        for (size_t index = 0; index < originals[n].size(); ++index)
            CPPUNIT_ASSERT_EQUAL(originals[n][index].get(), page->GetObj(index));
    }
    for (const auto& contact : contacts)
        CPPUNIT_ASSERT_EQUAL(0u, contact->changes);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testPdfFastUnchangedPageExport)
{
    auto library = vcl::pdf::PDFiumLibrary::get();
    if (!library)
        return;
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    struct Exported
    {
        std::vector<BitmapChecksum> pixels;
        std::vector<int> textObjects;
    };
    auto exportPdf = [&](bool tagged, const OUString& range = OUString(), bool lossless = false) {
        uno::Sequence<beans::PropertyValue> data{
            comphelper::makePropertyValue(u"UseTaggedPDF"_ustr, tagged),
            comphelper::makePropertyValue(u"UseLosslessCompression"_ustr, lossless),
            comphelper::makePropertyValue(u"ExportBookmarks"_ustr, false),
            comphelper::makePropertyValue(u"PageRange"_ustr, range),
        };
        uno::Reference<frame::XStorable> storable(mxComponent, uno::UNO_QUERY_THROW);
        storable->storeToURL(
            maTempFile.GetURL(),
            { comphelper::makePropertyValue(u"FilterName"_ustr, u"draw_pdf_Export"_ustr),
              comphelper::makePropertyValue(u"FilterData"_ustr, data) });
        auto pdf = parsePDFExport();
        CPPUNIT_ASSERT(pdf);
        Exported result;
        for (int i = 0; i < pdf->getPageCount(); ++i)
        {
            auto page = pdf->openPage(i);
            CPPUNIT_ASSERT(page);
            int texts = 0;
            for (int n = 0; n < page->getObjectCount(); ++n)
                if (page->getObject(n)->getType() == vcl::pdf::PDFPageObjectType::Text)
                    ++texts;
            result.textObjects.push_back(texts);
            int width = 400;
            int height = 300;
            auto bitmap = library->createBitmap(width, height, false);
            CPPUNIT_ASSERT(bitmap);
            bitmap->fillRect(0, 0, width, height, 0xffffffff);
            bitmap->renderPageBitmap(pdf.get(), page.get(), 0, 0, width, height);
            result.pixels.push_back(bitmap->createBitmapFromBuffer().GetChecksum());
        }
        return result;
    };
    // Native reference from a separate fresh model, before attaching PDF Fast.
    CPPUNIT_ASSERT(!doc->GetPdfFastDocument());
    const auto native = exportPdf(false);
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    SdrView view(*doc);
    // Exporting an untouched source must never ask PDFium to decompose it.
    std::vector<std::shared_ptr<VectorGraphicData>> sources;
    for (sal_uInt16 n = 0; n < 2; ++n)
    {
        auto* page = doc->GetSdPage(n, PageKind::Standard);
        auto* pv = view.ShowSdrPage(page);
        const auto* graphic = controller->GetUnchangedPageGraphic(*page, *pv);
        CPPUNIT_ASSERT(graphic);
        sources.push_back(graphic->GetGraphic().getVectorGraphicData());
        CPPUNIT_ASSERT(!sources.back()->isPrimitiveSequenceCreated());
    }
    const auto raw = exportPdf(false);
    for (const auto& source : sources)
        CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    const auto lossless = exportPdf(false, OUString(), true);
    CPPUNIT_ASSERT(raw.pixels == lossless.pixels);
    for (const auto& source : sources)
        CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    CPPUNIT_ASSERT(raw.pixels == native.pixels);
    CPPUNIT_ASSERT_EQUAL(std::size_t(3), raw.pixels.size());
    // Compare with the existing raw-PDF export at the same page order. Native
    // print-area sizing and PDFium resource caching can depend on that order.
    doc->SdrModel::MovePage(3, 1);
    doc->SdrModel::MovePage(4, 2);
    const auto rawReordered = exportPdf(false);
    const auto rawRange = exportPdf(false, u"1"_ustr);
    doc->SdrModel::MovePage(3, 1);
    doc->SdrModel::MovePage(4, 2);
    for (sal_uInt16 i = 0; i < 2; ++i)
    {
        auto* page = doc->GetSdPage(i, PageKind::Standard);
        auto* pageView = view.ShowSdrPage(page);
        controller->PrepareCurrentPage(view);
        CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    }
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* second = doc->GetSdPage(1, PageKind::Standard);
    const auto count = first->GetObjCount();
    const auto undo = doc->GetSdrUndoManager()->GetUndoActionCount();
    doc->SdrModel::SetChanged(false);
    model->GetDocShell()->SetModified(false);
    const auto unchanged = exportPdf(false);
    CPPUNIT_ASSERT(raw.pixels == unchanged.pixels);
    CPPUNIT_ASSERT(raw.textObjects == unchanged.textObjects);
    CPPUNIT_ASSERT_EQUAL(count, first->GetObjCount());
    CPPUNIT_ASSERT_EQUAL(undo, doc->GetSdrUndoManager()->GetUndoActionCount());
    CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
    // Range selection follows final page order, not source PDF page indices.
    doc->SdrModel::MovePage(3, 1);
    doc->SdrModel::MovePage(4, 2);
    const auto reordered = exportPdf(false);
    CPPUNIT_ASSERT(rawReordered.pixels == reordered.pixels);
    CPPUNIT_ASSERT(rawReordered.textObjects == reordered.textObjects);
    const auto range = exportPdf(false, u"1"_ustr);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), range.pixels.size());
    CPPUNIT_ASSERT_EQUAL(rawRange.pixels[0], range.pixels[0]);
    // Untouched pages keep source PDF objects even when ordinary tags are on.
    const auto tagged = exportPdf(true);
    CPPUNIT_ASSERT(tagged.pixels == reordered.pixels);
    CPPUNIT_ASSERT(tagged.textObjects == reordered.textObjects);
    first->GetObj(0)->Move(Size(700, 0));
    doc->SdrModel::SetChanged(false); // A save/reset must not establish a new baseline.
    model->GetDocShell()->SetModified(false);
    auto* firstView = view.ShowSdrPage(first);
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*first, *firstView));
    auto* secondView = view.ShowSdrPage(second);
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*second, *secondView));
    const auto modified = exportPdf(false);
    CPPUNIT_ASSERT_EQUAL(rawReordered.pixels[0], modified.pixels[0]);
    CPPUNIT_ASSERT(modified.pixels[1] != rawReordered.pixels[1]);
    CPPUNIT_ASSERT(modified.textObjects[1] > 0);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testPdfFastMixedTaggedExport)
{
    auto library = vcl::pdf::PDFiumLibrary::get();
    if (!library)
        return;
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* second = doc->GetSdPage(1, PageKind::Standard);
    second->GetObj(0)->SetTitle(u"Source page"_ustr);
    second->GetObj(0)->SetDescription(u"Page description"_ustr);
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* fast = doc->GetPdfFastDocument();
    SdrView view(*doc);
    auto* secondView = view.ShowSdrPage(second);
    const auto* original = fast->GetUnchangedPageGraphic(*second, *secondView);
    CPPUNIT_ASSERT(original);
    auto source = original->GetGraphic().getVectorGraphicData();
    CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    struct Exported
    {
        std::vector<BitmapChecksum> pixels;
        std::vector<std::vector<OUString>> tags;
        std::vector<OUString> figureAlt;
        std::vector<int> figureMCIDs;
    };
    auto exportPdf = [&](bool tagged, const OUString& range = OUString(), bool ua = false) {
        uno::Reference<frame::XStorable> storable(mxComponent, uno::UNO_QUERY_THROW);
        storable->storeToURL(
            maTempFile.GetURL(),
            { comphelper::makePropertyValue(u"FilterName"_ustr, u"draw_pdf_Export"_ustr),
              comphelper::makePropertyValue(
                  u"FilterData"_ustr,
                  uno::Sequence<beans::PropertyValue>{
                      comphelper::makePropertyValue(u"UseTaggedPDF"_ustr, tagged),
                      comphelper::makePropertyValue(u"PDFUACompliance"_ustr, ua),
                      comphelper::makePropertyValue(u"ExportBookmarks"_ustr, false),
                      comphelper::makePropertyValue(u"PageRange"_ustr, range) }) });
        auto pdf = parsePDFExport();
        CPPUNIT_ASSERT(pdf);
        Exported result;
        for (int i = 0; i < pdf->getPageCount(); ++i)
        {
            auto page = pdf->openPage(i);
            CPPUNIT_ASSERT(page);
            int width = 400, height = 300;
            auto bitmap = library->createBitmap(width, height, false);
            CPPUNIT_ASSERT(bitmap);
            bitmap->fillRect(0, 0, width, height, 0xffffffff);
            bitmap->renderPageBitmap(pdf.get(), page.get(), 0, 0, width, height);
            result.pixels.push_back(bitmap->createBitmapFromBuffer().GetChecksum());
            result.tags.emplace_back();
            auto collect = [&](auto&& self, vcl::pdf::PDFiumStructureElement& element) -> void {
                result.tags.back().push_back(element.getType());
                if (element.getType() == "Figure")
                {
                    result.figureAlt.push_back(element.getAltText());
                    for (int n = 0; n < element.getNumberOfChildren(); ++n)
                    {
                        const int mcid = element.getChildMarkedContentID(n);
                        if (mcid >= 0)
                            result.figureMCIDs.push_back(mcid);
                    }
                }
                for (int n = 0; n < element.getNumberOfChildren(); ++n)
                    if (auto child = element.getChild(n))
                        self(self, *child);
            };
            if (auto tree = page->getStructureTree())
                for (int n = 0; n < tree->getNumberOfChildren(); ++n)
                    if (auto child = tree->getChild(n))
                        collect(collect, *child);
        }
        return result;
    };
    const auto raw = exportPdf(false);
    view.ShowSdrPage(first);
    fast->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(first->GetObjCount() > 1);
    first->GetObj(0)->Move(Size(700, 0));
    // Document-wide saved/modified flags must not decide which page is reused.
    model->GetDocShell()->SetModified(false);
    doc->SdrModel::SetChanged(false);
    auto* firstView = view.ShowSdrPage(first);
    CPPUNIT_ASSERT(!fast->GetUnchangedPageGraphic(*first, *firstView));
    secondView = view.ShowSdrPage(second);
    CPPUNIT_ASSERT(fast->GetUnchangedPageGraphic(*second, *secondView));
    const auto mixed = exportPdf(true);
    CPPUNIT_ASSERT_EQUAL(std::size_t(3), mixed.pixels.size());
    CPPUNIT_ASSERT(raw.pixels[0] != mixed.pixels[0]);
    CPPUNIT_ASSERT_EQUAL(raw.pixels[1], mixed.pixels[1]);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), second->GetObjCount());
    CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    // Modified text retains native paragraph tags. Source pages are real tagged
    // Figures with MCIDs, not unmarked content in a nominally tagged document.
    CPPUNIT_ASSERT(std::find(mixed.tags[0].begin(), mixed.tags[0].end(), u"P"_ustr)
                   != mixed.tags[0].end());
    CPPUNIT_ASSERT(std::find(mixed.tags[1].begin(), mixed.tags[1].end(), u"Figure"_ustr)
                   != mixed.tags[1].end());
    CPPUNIT_ASSERT(std::find(mixed.figureAlt.begin(), mixed.figureAlt.end(),
                             u"Source page - Page description"_ustr)
                   != mixed.figureAlt.end());
    CPPUNIT_ASSERT(!mixed.figureMCIDs.empty());
    const auto repeated = exportPdf(true);
    CPPUNIT_ASSERT(mixed.pixels == repeated.pixels);
    CPPUNIT_ASSERT(mixed.tags == repeated.tags);
    doc->SdrModel::MovePage(3, 1);
    doc->SdrModel::MovePage(4, 2);
    // Native export sizing depends on the first page; compare the same order.
    const auto rangeReference = exportPdf(false, u"1"_ustr);
    const auto range = exportPdf(true, u"1"_ustr);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), range.pixels.size());
    CPPUNIT_ASSERT_EQUAL(rangeReference.pixels[0], range.pixels[0]);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), range.figureMCIDs.size());
    CPPUNIT_ASSERT_EQUAL(0, range.figureMCIDs[0]);
    CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    // Strict accessibility output deliberately keeps native Draw tagging.
    exportPdf(true, u"1"_ustr, true);
    CPPUNIT_ASSERT(source->isPrimitiveSequenceCreated());
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testPdfFastExportCorrectnessBarriers)
{
    if (!vcl::pdf::PDFiumLibrary::get())
        return;
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    SdrView view(*doc);
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    auto* pageView = view.ShowSdrPage(page);
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    const auto size = page->GetSize();
    page->SetSize(Size(size.Width() + 100, size.Height()));
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*page, *pageView));
    page->SetSize(size);
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    const auto layers = pageView->GetPrintableLayers();
    pageView->SetPrintableLayers(SdrLayerIDSet());
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*page, *pageView));
    pageView->SetPrintableLayers(layers);
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    // Nbc edits bypass hints; immutable primitive comparison is the second gate.
    page->GetObj(0)->NbcMove(Size(100, 0));
    page->GetObj(0)->GetViewContact().ActionChanged();
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*page, *pageView));
    page->GetObj(0)->NbcMove(Size(-100, 0));
    page->GetObj(0)->GetViewContact().ActionChanged();
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    // After reversing the edit, the immutable baseline proves that the page is
    // unchanged again and source reuse is safe.
    page->GetObj(0)->Move(Size(100, 0));
    page->GetObj(0)->Move(Size(-100, 0));
    CPPUNIT_ASSERT(controller->GetUnchangedPageGraphic(*page, *pageView));
    // Silent additions before conversion must never disappear from export.
    auto* raw = doc->GetSdPage(1, PageKind::Standard);
    raw->NbcInsertObject(page->GetObj(0)->CloneSdrObject(*doc).get());
    pageView = view.ShowSdrPage(raw);
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*raw, *pageView));
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testPdfFastUnchangedLinksExport)
{
    auto library = vcl::pdf::PDFiumLibrary::get();
    if (!library)
        return;
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    auto exportPdf = [&](const std::vector<OUString>& urls, const OUString& range = OUString(),
                         bool tagged = false) {
        uno::Reference<frame::XStorable> storable(mxComponent, uno::UNO_QUERY_THROW);
        storable->storeToURL(
            maTempFile.GetURL(),
            { comphelper::makePropertyValue(u"FilterName"_ustr, u"draw_pdf_Export"_ustr),
              comphelper::makePropertyValue(
                  u"FilterData"_ustr,
                  uno::Sequence<beans::PropertyValue>{
                      comphelper::makePropertyValue(u"UseTaggedPDF"_ustr, tagged),
                      comphelper::makePropertyValue(u"ExportBookmarks"_ustr, false),
                      comphelper::makePropertyValue(u"PageRange"_ustr, range) }) });
        auto pdf = parsePDFExport();
        CPPUNIT_ASSERT(pdf);
        CPPUNIT_ASSERT_EQUAL(int(urls.size()), pdf->getPageCount());
        std::vector<BitmapChecksum> pixels;
        for (int i = 0; i < pdf->getPageCount(); ++i)
        {
            auto page = pdf->openPage(i);
            CPPUNIT_ASSERT(page);
            CPPUNIT_ASSERT_EQUAL(urls[i].isEmpty() ? 0 : 1, page->getAnnotationCount());
            int index = 0;
            auto link = page->enumerateLink(&index, pdf.get());
            if (!urls[i].isEmpty())
            {
                CPPUNIT_ASSERT(link);
                CPPUNIT_ASSERT_EQUAL(urls[i], link->getURIPath());
                // Rectangles follow their source page. Native Draw uses rounded
                // 1/100 mm page/annotation coordinates on materialized pages.
                const auto rect = link->getRectangle();
                CPPUNIT_ASSERT_DOUBLES_EQUAL(urls[i].endsWith("first") ? 10.0 : 15.0,
                                             rect.getMinX(), 0.04);
                CPPUNIT_ASSERT_DOUBLES_EQUAL(20.0, rect.getMinY(), 0.04);
                CPPUNIT_ASSERT(!page->enumerateLink(&index, pdf.get()));
            }
            else
                CPPUNIT_ASSERT(!link);
            int width = 400, height = 300;
            auto bitmap = library->createBitmap(width, height, false);
            CPPUNIT_ASSERT(bitmap);
            bitmap->fillRect(0, 0, width, height, 0xffffffff);
            bitmap->renderPageBitmap(pdf.get(), page.get(), 0, 0, width, height);
            pixels.push_back(bitmap->createBitmapFromBuffer().GetChecksum());
        }
        return pixels;
    };
    const OUString firstURL = u"https://example.test/first"_ustr;
    const OUString secondURL = u"https://example.test/second"_ustr;
    loadFromFile(u"pdf/pdffast-linked-pages.pdf");
    // The second source link omits optional /Type: ordinary Draw currently
    // skips it. PDF Fast must preserve both links without changing Draw.
    const auto native = exportPdf({ firstURL, OUString(), OUString() });
    loadFromFile(u"pdf/pdffast-linked-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* fast = doc->GetPdfFastDocument();
    SdrView view(*doc);
    std::vector<std::shared_ptr<VectorGraphicData>> sources;
    for (sal_uInt16 i = 0; i < 2; ++i)
    {
        auto* page = doc->GetSdPage(i, PageKind::Standard);
        auto* pv = view.ShowSdrPage(page);
        const auto* source = fast->GetUnchangedPageGraphic(*page, *pv);
        CPPUNIT_ASSERT(source);
        sources.push_back(source->GetGraphic().getVectorGraphicData());
        CPPUNIT_ASSERT(!sources.back()->isPrimitiveSequenceCreated());
    }
    CPPUNIT_ASSERT(native == exportPdf({ firstURL, secondURL, OUString() }));
    for (const auto& source : sources)
        CPPUNIT_ASSERT(!source->isPrimitiveSequenceCreated());
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* pv = view.ShowSdrPage(first);
    const auto links = first->getLinkAnnotations();
    // Even setters without notifications must not allow a stale metadata copy.
    first->setLinkAnnotations({});
    CPPUNIT_ASSERT(!fast->GetUnchangedPageGraphic(*first, *pv));
    first->setLinkAnnotations(links);
    fast->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(first->GetObjCount() > 1);
    CPPUNIT_ASSERT(fast->GetUnchangedPageGraphic(*first, *pv));
    CPPUNIT_ASSERT(native == exportPdf({ firstURL, secondURL, OUString() }));
    // A new writer must register its own source, with no stale indices across saves.
    CPPUNIT_ASSERT(native == exportPdf({ firstURL, secondURL, OUString() }));
    first->GetObj(0)->Move(Size(700, 0));
    CPPUNIT_ASSERT(!fast->GetUnchangedPageGraphic(*first, *pv));
    exportPdf({ firstURL, secondURL, OUString() });
    exportPdf({ firstURL, secondURL, OUString() }, OUString(), true);
    first->setLinkAnnotations({});
    exportPdf({ OUString(), secondURL, OUString() });
    first->setLinkAnnotations(links);
    first->GetObj(0)->Move(Size(-700, 0));
    CPPUNIT_ASSERT(fast->GetUnchangedPageGraphic(*first, *pv));
    doc->SdrModel::MovePage(3, 1);
    doc->SdrModel::MovePage(4, 2);
    exportPdf({ secondURL, firstURL, OUString() });
    exportPdf({ secondURL }, u"1"_ustr);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testPdfFastAnnotatedExportFallback)
{
    if (!vcl::pdf::PDFiumLibrary::get())
        return;
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"PdfWithAnnotation.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    SdrView view(*doc);
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    auto* pageView = view.ShowSdrPage(page);
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(!page->getAnnotations().empty());
    CPPUNIT_ASSERT(!controller->GetUnchangedPageGraphic(*page, *pageView));
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testDeferredEditPreservesPageFormat)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    const Size size(page->GetSize().Width() + 1000, page->GetSize().Height() + 2000);
    page->SetSize(size);
    page->SetBorder(101, 202, 303, 404);
    SdrView view(*doc);
    view.ShowSdrPage(page);
    doc->GetPdfFastDocument()->PrepareCurrentPage(view);
    CPPUNIT_ASSERT_EQUAL(size, page->GetSize());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(101), page->GetLeftBorder());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(202), page->GetUpperBorder());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(303), page->GetRightBorder());
    CPPUNIT_ASSERT_EQUAL(sal_Int32(404), page->GetLowerBorder());
    CPPUNIT_ASSERT(page->GetObjCount() > 1);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testDeferredRenderingMatchesNativeImport)
{
    EnvVarGuard nativeImport("LO_IMPORT_USE_PDFIUM", nullptr);
    struct ExpectedPage
    {
        Size size;
        std::vector<tools::Rectangle> bounds;
        Bitmap pixels;
    };
    std::vector<ExpectedPage> expected;
    // Reference the default desktop Poppler/XML path, not PDFium break-apart.
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    {
        auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
        CPPUNIT_ASSERT(model);
        auto* doc = model->GetDocShell()->GetDoc();
        CPPUNIT_ASSERT_EQUAL(sal_uInt16(3), doc->GetSdPageCount(PageKind::Standard));
        SdrView view(*doc);
        for (sal_uInt16 index = 0; index < 3; ++index)
        {
            auto* page = doc->GetSdPage(index, PageKind::Standard);
            ExpectedPage snapshot{ page->GetSize(), {}, {} };
            view.ShowSdrPage(page);
            view.MarkAll();
            snapshot.pixels = view.GetMarkedObjBitmap();
            dumpPdfBitmap(u"native-"_ustr + OUString::number(index), snapshot.pixels);
            view.UnmarkAllObj();
            for (std::size_t object = 0; object < page->GetObjCount(); ++object)
                snapshot.bounds.push_back(page->GetObj(object)->GetSnapRect());
            expected.push_back(std::move(snapshot));
        }
    }
    CPPUNIT_ASSERT(!expected[0].pixels.IsEmpty());
    CPPUNIT_ASSERT(!expected[1].pixels.IsEmpty());
    mxComponent->dispose();
    mxComponent.clear();

    EnvVarGuard pdfiumPreview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    SdrView view(*doc);
    for (sal_uInt16 index = 0; index < 3; ++index)
    {
        auto* page = doc->GetSdPage(index, PageKind::Standard);
        rtl::Reference<SdrObject> original = page->GetObj(0);
        const Size originalSize = page->GetSize();
        view.ShowSdrPage(page);
        doc->GetPdfFastDocument()->PrepareCurrentPage(view);
        CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() != page);
        CPPUNIT_ASSERT_EQUAL(originalSize, page->GetSize());
        // PDFium rounds page dimensions to 1/100 mm while Poppler truncates.
        // Keep the target geometry; native object pixels must still match.
        CPPUNIT_ASSERT(std::abs(expected[index].size.Width() - page->GetSize().Width()) <= 1);
        CPPUNIT_ASSERT(std::abs(expected[index].size.Height() - page->GetSize().Height()) <= 1);
        CPPUNIT_ASSERT_EQUAL(expected[index].bounds.size(), page->GetObjCount());
        // The temporary import shell has closed. Compare real rendered pixels,
        // including form transforms, shading, text and an embedded image.
        // The blank third page must also replace its original PDF graphic.
        view.MarkAll();
        const Bitmap actual = view.GetMarkedObjBitmap();
        dumpPdfBitmap(u"deferred-"_ustr + OUString::number(index), actual);
        for (std::size_t object = 0; object < page->GetObjCount(); ++object)
            CPPUNIT_ASSERT_EQUAL(expected[index].bounds[object],
                                 page->GetObj(object)->GetSnapRect());
        CPPUNIT_ASSERT_EQUAL(expected[index].pixels.GetSizePixel(), actual.GetSizePixel());
        CPPUNIT_ASSERT_EQUAL(expected[index].pixels.GetChecksum(), actual.GetChecksum());
        view.UnmarkAllObj();
        CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
        if (index < 2)
            CPPUNIT_ASSERT_EQUAL(std::size_t(1),
                                 doc->GetSdPage(index + 1, PageKind::Standard)->GetObjCount());
    }
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testFailedNativePageImportKeepsPreview)
{
    EnvVarGuard pdfiumPreview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    rtl::Reference<SdrGrafObj> original = dynamic_cast<SdrGrafObj*>(page->GetObj(0));
    CPPUNIT_ASSERT(original.is());
    const Graphic valid = original->GetGraphic();
    // Retain the PDF bytes while selecting a nonexistent source page.
    original->SetGraphic(Graphic(valid.GetSharedGfxLink(), 999));
    model->GetDocShell()->SetModified(false);
    SdrView view(*doc);
    view.ShowSdrPage(page);
    const auto undoCount = doc->GetSdrUndoManager()->GetUndoActionCount();
    doc->GetPdfFastDocument()->PrepareCurrentPage(view);
    CPPUNIT_ASSERT_EQUAL(std::size_t(1), page->GetObjCount());
    CPPUNIT_ASSERT_EQUAL(static_cast<SdrObject*>(original.get()), page->GetObj(0));
    CPPUNIT_ASSERT(original->IsMoveProtect());
    CPPUNIT_ASSERT(original->IsResizeProtect());
    CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
    CPPUNIT_ASSERT_EQUAL(undoCount, doc->GetSdrUndoManager()->GetUndoActionCount());
    original->SetGraphic(valid);
    model->GetDocShell()->SetModified(false);
    doc->GetPdfFastDocument()->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() != page);
    CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
    CPPUNIT_ASSERT_EQUAL(undoCount, doc->GetSdrUndoManager()->GetUndoActionCount());
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testDeferredPageCopyAndCompatibleSave)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    const auto count = doc->GetSdPageCount(PageKind::Standard);
    const uno::Reference<drawing::XDrawPageDuplicator> duplicator(mxComponent,
                                                                  uno::UNO_QUERY_THROW);
    const uno::Reference<drawing::XDrawPage> source(model->getDrawPages()->getByIndex(0),
                                                    uno::UNO_QUERY_THROW);
    const auto duplicate = duplicator->duplicate(source);
    CPPUNIT_ASSERT(duplicate.is());
    CPPUNIT_ASSERT_EQUAL(sal_uInt16(count + 1), doc->GetSdPageCount(PageKind::Standard));
    auto* copy = doc->GetSdPage(1, PageKind::Standard);
    {
        rtl::Reference<SdrObject> original = copy->GetObj(0);
        rtl::Reference<SdrObject> firstOriginal = first->GetObj(0);
        SdrView view(*doc);
        view.ShowSdrPage(copy);
        doc->GetPdfFastDocument()->PrepareCurrentPage(view);
        CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() != copy);
        CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
        // Save through the real native ODG path. Untouched PDF pages must also
        // become editable; reopening must not require the PDF-only controller.
        view.HideSdrPage();
        doc->GetPdfFastDocument()->ForgetView(view);
    } // Destroy the view and retained objects before disposing the old model.
    saveAndReload(TestFilter::ODG);
    model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT_EQUAL(sal_uInt16(count + 1), doc->GetSdPageCount(PageKind::Standard));
    CPPUNIT_ASSERT(!doc->GetPdfFastDocument());
    for (sal_uInt16 n = 0; n < doc->GetSdPageCount(PageKind::Standard); ++n)
        for (size_t object = 0; object < doc->GetSdPage(n, PageKind::Standard)->GetObjCount();
             ++object)
        {
            const auto* graphic = dynamic_cast<const SdrGrafObj*>(
                doc->GetSdPage(n, PageKind::Standard)->GetObj(object));
            // Inspect the source type without decoding graphic contents.
            const auto link = graphic ? graphic->GetGraphic().GetSharedGfxLink() : nullptr;
            CPPUNIT_ASSERT(!link || link->GetType() != GfxLinkType::NativePdf);
        }
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testDeferredEditRequestTracksViewAndReadOnly)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* shell = model->GetDocShell();
    auto* doc = shell->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* second = doc->GetSdPage(1, PageKind::Standard);
    rtl::Reference<SdrObject> firstOriginal = first->GetObj(0);
    rtl::Reference<SdrObject> secondOriginal = second->GetObj(0);
    SdrView view(*doc);
    SdrView otherView(*doc);
    comphelper::ScopeGuard forget([&] { controller->ForgetView(view); });
    // Opening a PDF and selecting its first page must remain a read-only
    // preview. The explicit request is ignored until edit mode is entered.
    shell->SetReadOnlyUI(true);
    view.ShowSdrPage(first);
    controller->RequestCurrentPage(view);
    CPPUNIT_ASSERT(controller->GetEditState(view) == sd::PdfFastDocument::EditState::Ready);
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
    view.HideSdrPage();
    shell->SetReadOnlyUI(false);
    view.ShowSdrPage(first);
    otherView.ShowSdrPage(second);
    controller->RequestCurrentPage(view);
    // The UI entry point must return before conversion, keeping its preview.
    CPPUNIT_ASSERT(controller->GetEditState(view) == sd::PdfFastDocument::EditState::Preparing);
    CPPUNIT_ASSERT(view.GetSdrPageView()->IsReadOnly());
    CPPUNIT_ASSERT(!otherView.GetSdrPageView()->IsReadOnly());
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
    view.HideSdrPage();
    view.ShowSdrPage(second);
    controller->RequestCurrentPage(view);
    shell->SetReadOnlyUI(true);
    controller->RequestCurrentPage(view);
    CPPUNIT_ASSERT(controller->GetEditState(view) == sd::PdfFastDocument::EditState::Ready);
    const auto cancelledUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < cancelledUntil)
    {
        Application::Reschedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
    CPPUNIT_ASSERT(secondOriginal->getSdrPageFromSdrObject() == second);

    shell->SetReadOnlyUI(false);
    shell->SetModified(false);
    const auto undoCount = doc->GetSdrUndoManager()->GetUndoActionCount();
    // ModeChanged also queues the real Draw view's page 1. This test drives a
    // separate view on page 2, so cancel that independent request.
    controller->ForgetView(*shell->GetViewShell()->GetDrawView());
    controller->RequestCurrentPage(view);
    for (int n = 0; n < 8; ++n)
    {
        controller->NoteScroll();
        Application::Reschedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CPPUNIT_ASSERT(secondOriginal->getSdrPageFromSdrObject() == second);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (secondOriginal->getSdrPageFromSdrObject() == second
           && std::chrono::steady_clock::now() < deadline)
    {
        Application::Reschedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CPPUNIT_ASSERT(secondOriginal->getSdrPageFromSdrObject() != second);
    CPPUNIT_ASSERT(controller->GetEditState(view) == sd::PdfFastDocument::EditState::Ready);
    CPPUNIT_ASSERT(!view.GetSdrPageView()->IsReadOnly());
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
    CPPUNIT_ASSERT(!shell->IsModified());
    CPPUNIT_ASSERT_EQUAL(undoCount, doc->GetSdrUndoManager()->GetUndoActionCount());
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testAdjacentEditPrefetchDoesNotMaterializePages)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* shell = model->GetDocShell();
    auto* doc = shell->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* fast = doc->GetPdfFastDocument();
    shell->SetReadOnlyUI(false);
    fast->ForgetView(*shell->GetViewShell()->GetDrawView());
    SdrView view(*doc);
    comphelper::ScopeGuard forget([&] { fast->ForgetView(view); });
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* middle = doc->GetSdPage(1, PageKind::Standard);
    auto* last = doc->GetSdPage(2, PageKind::Standard);
    rtl::Reference<SdrObject> firstOriginal = first->GetObj(0);
    rtl::Reference<SdrObject> lastOriginal = last->GetObj(0);
    view.ShowSdrPage(middle);
    fast->PrepareCurrentPage(view);
    shell->SetModified(false);
    const auto undo = doc->GetSdrUndoManager()->GetUndoActionCount();
    fast->RequestCurrentPage(view);
    auto pumpUntil = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!condition() && std::chrono::steady_clock::now() < deadline)
        {
            Application::Reschedule();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CPPUNIT_ASSERT(condition());
    };
    pumpUntil([&] { return fast->HasPrefetchedPage(*first) && fast->HasPrefetchedPage(*last); });
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() == first);
    CPPUNIT_ASSERT(lastOriginal->getSdrPageFromSdrObject() == last);
    CPPUNIT_ASSERT(!shell->IsModified());
    CPPUNIT_ASSERT_EQUAL(undo, doc->GetSdrUndoManager()->GetUndoActionCount());
    view.ShowSdrPage(first);
    fast->NoteScroll();
    fast->RequestCurrentPage(view);
    CPPUNIT_ASSERT(fast->HasPrefetchedPage(*first));
    pumpUntil([&] { return fast->GetEditState(view) == sd::PdfFastDocument::EditState::Ready; });
    CPPUNIT_ASSERT(firstOriginal->getSdrPageFromSdrObject() != first);
    CPPUNIT_ASSERT(!fast->HasPrefetchedPage(*first));
    CPPUNIT_ASSERT(!shell->IsModified());
    CPPUNIT_ASSERT_EQUAL(undo, doc->GetSdrUndoManager()->GetUndoActionCount());
    fast->SetEditMode(false);
    CPPUNIT_ASSERT(!fast->HasPrefetchedPage(*last));
    CPPUNIT_ASSERT(lastOriginal->getSdrPageFromSdrObject() == last);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testSaveWhileScrollDefersEdit)
{
    EnvVarGuard preview("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/pdffast-native-pages.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* shell = model->GetDocShell();
    auto* doc = shell->GetDoc();
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    shell->SetReadOnlyUI(false);
    controller->ForgetView(*shell->GetViewShell()->GetDrawView());
    auto* page = doc->GetSdPage(0, PageKind::Standard);
    rtl::Reference<SdrObject> original = page->GetObj(0);
    SdrView view(*doc);
    comphelper::ScopeGuard forget([&] { controller->ForgetView(view); });
    view.ShowSdrPage(page);
    controller->RequestCurrentPage(view);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        controller->NoteScroll();
        Application::Reschedule();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() == page);
    CPPUNIT_ASSERT(controller->PrepareForSave());
    CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() != page);
    CPPUNIT_ASSERT(controller->GetEditState(view) == sd::PdfFastDocument::EditState::Ready);
}

CPPUNIT_TEST_FIXTURE(SdrPdfImportTest, testNativeDeferredPageEditing)
{
    EnvVarGuard usePdfium("LO_IMPORT_USE_PDFIUM", "1");
    loadFromFile(u"pdf/multipage.pdf");
    auto* model = dynamic_cast<SdXImpressDocument*>(mxComponent.get());
    CPPUNIT_ASSERT(model);
    auto* doc = model->GetDocShell()->GetDoc();
    CPPUNIT_ASSERT(doc->GetSdPageCount(PageKind::Standard) >= 2);
    // Production attachment is desktop-only. Explicitly attach the same adapter
    // here so this headless test exercises its real model conversion/lifetime.
    CPPUNIT_ASSERT(sd::PdfFastDocument::Attach(*doc));
    auto* controller = doc->GetPdfFastDocument();
    CPPUNIT_ASSERT(controller);
    auto* first = doc->GetSdPage(0, PageKind::Standard);
    auto* second = doc->GetSdPage(1, PageKind::Standard);
    rtl::Reference<SdrObject> originalFirst(first->GetObj(0));
    rtl::Reference<SdrObject> originalSecond(second->GetObj(0));
    const auto secondCount = second->GetObjCount();
    SdrView view(*doc);
    view.ShowSdrPage(first);
    doc->EnableUndo(true);
    model->GetDocShell()->SetModified(false);
    auto* undo = doc->GetSdrUndoManager();
    CPPUNIT_ASSERT(undo);
    const auto initialUndo = undo->GetUndoActionCount();
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(!originalFirst->getSdrPageFromSdrObject());
    CPPUNIT_ASSERT(first->GetObjCount() > 0);
    CPPUNIT_ASSERT(originalSecond->getSdrPageFromSdrObject() == second);
    CPPUNIT_ASSERT_EQUAL(secondCount, second->GetObjCount());
    CPPUNIT_ASSERT(!model->GetDocShell()->IsModified());
    CPPUNIT_ASSERT(doc->IsUndoEnabled());
    CPPUNIT_ASSERT_EQUAL(initialUndo, undo->GetUndoActionCount());
    const auto firstCount = first->GetObjCount();
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT_EQUAL(firstCount, first->GetObjCount());

    // A real native edit on page 1 must survive lazy conversion of page 2,
    // including its object identity and undo action.
    auto* moved = first->GetObj(0);
    const Point before = moved->GetSnapRect().TopLeft();
    const Size delta(100, 50);
    doc->BegUndo();
    doc->AddUndo(std::make_unique<SdrUndoMoveObj>(*moved, delta));
    moved->Move(delta);
    doc->EndUndo();
    model->GetDocShell()->SetModified(true);
    const auto editedUndo = undo->GetUndoActionCount();
    view.HideSdrPage();
    view.ShowSdrPage(second);
    controller->PrepareCurrentPage(view);
    CPPUNIT_ASSERT(!originalSecond->getSdrPageFromSdrObject());
    CPPUNIT_ASSERT(model->GetDocShell()->IsModified());
    CPPUNIT_ASSERT_EQUAL(editedUndo, undo->GetUndoActionCount());
    CPPUNIT_ASSERT(undo->Undo());
    CPPUNIT_ASSERT_EQUAL(before, moved->GetSnapRect().TopLeft());
    // view dies before the document: the controller must also safely cancel/join
    // its memory-backed worker during the ordinary fixture document teardown.
}
#endif

CPPUNIT_PLUGIN_IMPLEMENT();

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
