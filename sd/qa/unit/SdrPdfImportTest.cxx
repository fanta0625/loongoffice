/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <test/unoapi_test.hxx>
#include <optional>
#include <string>
#include <vector>
#include <config_features.h>
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
#include <PdfFastDocument.hxx>
#include <drawdoc.hxx>
#include <svx/svdview.hxx>
#include <svx/svdundo.hxx>
#include <svl/undo.hxx>
#endif

#include <comphelper/scopeguard.hxx>
#include <comphelper/propertysequence.hxx>

#include <unotools/tempfile.hxx>
#include <unotools/mediadescriptor.hxx>
#include <svx/svdograf.hxx>
#include <editeng/outlobj.hxx>
#include <editeng/editobj.hxx>
#include <vcl/filter/PDFiumLibrary.hxx>
#include <vcl/pdf/PDFAnnotationSubType.hxx>
#include <vcl/bitmap.hxx>

#include <Annotation.hxx>
#include <DrawDocShell.hxx>
#include <ViewShell.hxx>
#include <sdpage.hxx>
#include <unomodel.hxx>

using namespace css;

namespace
{
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
    ~EnvVarGuard()
    {
        SetEnv(sVar, m_previous ? m_previous->c_str() : nullptr);
    }

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
        view.ShowSdrPage(page);
        doc->GetPdfFastDocument()->PrepareCurrentPage(view);
        CPPUNIT_ASSERT(original->getSdrPageFromSdrObject() != page);
        CPPUNIT_ASSERT_EQUAL(expected[index].size, page->GetSize());
        CPPUNIT_ASSERT_EQUAL(expected[index].bounds.size(), page->GetObjCount());
        // The temporary import shell has closed. Compare real rendered pixels,
        // including form transforms, shading, text and an embedded image.
        // The blank third page must also replace its original PDF graphic.
        view.MarkAll();
        const Bitmap actual = view.GetMarkedObjBitmap();
        for (std::size_t object = 0; object < page->GetObjCount(); ++object)
            CPPUNIT_ASSERT_EQUAL(expected[index].bounds[object], page->GetObj(object)->GetSnapRect());
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
    Graphic invalid = original->GetGraphic();
    invalid.setPageNumber(999); // Fail inside the real native filter/helper.
    original->SetGraphic(invalid);
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
