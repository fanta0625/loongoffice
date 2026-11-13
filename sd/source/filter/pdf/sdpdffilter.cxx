/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <sal/config.h>

#include <osl/process.h>
#include <sfx2/docfile.hxx>
#include <svx/svdograf.hxx>
#include <o3tl/safeint.hxx>

#include <sdpage.hxx>
#include <drawdoc.hxx>
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
#include <PdfFastDocument.hxx>
#include <vcl/svapp.hxx>
#include <chrono>
#include <sal/log.hxx>
#endif
#include <sdpdffilter.hxx>

#include <vcl/graph.hxx>
#include <vcl/pdfread.hxx>

#include <Annotation.hxx>
#include <DrawDocShell.hxx>
#include <unomodel.hxx>

#include <com/sun/star/document/XExporter.hpp>
#include <com/sun/star/document/XFilter.hpp>
#include <com/sun/star/lang/XInitialization.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <com/sun/star/office/XAnnotation.hpp>
#include <com/sun/star/text/XText.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>

#include <basegfx/polygon/b2dpolygontools.hxx>
#include <comphelper/lok.hxx>
#include <comphelper/scopeguard.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/propertysequence.hxx>
#include <unotools/streamwrap.hxx>
#include <unotools/tempfile.hxx>
#include <unotools/ucbstreamhelper.hxx>

using namespace css;

SdPdfFilter::SdPdfFilter(SfxMedium& rMedium, sd::DrawDocShell& rDocShell)
    : SdFilter(rMedium, rDocShell)
{
}

SdPdfFilter::~SdPdfFilter() {}

void SdPdfFilter::ApplyPageMetadata(SdPage& page, const vcl::PDFGraphicResult& rPDFGraphicResult)
{
    for (auto const& rPDFAnnotation : rPDFGraphicResult.GetAnnotations())
    {
        rtl::Reference<sdr::annotation::Annotation> xAnnotation = page.createAnnotation();

        xAnnotation->setAuthor(rPDFAnnotation.maAuthor);

        uno::Reference<text::XText> xText(xAnnotation->getTextRange());
        xText->setString(rPDFAnnotation.maText);
        // position is in mm not 100thmm
        geometry::RealPoint2D aUnoPosition(rPDFAnnotation.maRectangle.getMinX() / 100.0,
                                           rPDFAnnotation.maRectangle.getMinY() / 100.0);
        geometry::RealSize2D aUnoSize(rPDFAnnotation.maRectangle.getWidth() / 100.0,
                                      rPDFAnnotation.maRectangle.getHeight() / 100.0);
        xAnnotation->setPosition(aUnoPosition);
        xAnnotation->setSize(aUnoSize);
        xAnnotation->setDateTime(rPDFAnnotation.maDateTime);

        if (rPDFAnnotation.mpMarker)
        {
            sdr::annotation::CreationInfo aInfo;
            aInfo.maRectangle = rPDFAnnotation.maRectangle;

            if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Polygon)
            {
                aInfo.meType = sdr::annotation::AnnotationType::Polygon;

                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerPolygon*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.maPolygons.push_back(pMarker->maPolygon);
                aInfo.mnWidth = pMarker->mnWidth;
                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.mbColor = true;
                aInfo.maFillColor = pMarker->maFillColor;
                aInfo.mbFillColor = true;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Square)
            {
                aInfo.meType = sdr::annotation::AnnotationType::Square;

                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerSquare*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.mnWidth = pMarker->mnWidth;
                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.mbColor = true;
                aInfo.maFillColor = pMarker->maFillColor;
                aInfo.mbFillColor = true;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Circle)
            {
                aInfo.meType = sdr::annotation::AnnotationType::Circle;

                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerCircle*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.mnWidth = pMarker->mnWidth;
                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.mbColor = true;
                aInfo.maFillColor = pMarker->maFillColor;
                aInfo.mbFillColor = true;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Ink)
            {
                aInfo.meType = sdr::annotation::AnnotationType::Ink;

                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerInk*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.maPolygons = pMarker->maStrokes;

                aInfo.mnWidth = pMarker->mnWidth;
                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.mbColor = true;
                aInfo.maFillColor = pMarker->maFillColor;
                aInfo.mbFillColor = true;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Highlight)
            {
                aInfo.meType = sdr::annotation::AnnotationType::Highlight;

                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerHighlight*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.maColor.SetAlpha(0xFF - 0x90);
                aInfo.mbColor = true;

                aInfo.mnWidth = 1;

                aInfo.maFillColor = rPDFAnnotation.maColor;
                aInfo.maFillColor.SetAlpha(0xFF - 0x90);
                aInfo.mbFillColor = true;

                aInfo.maPolygons = pMarker->maQuads;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Line)
            {
                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerLine*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.meType = sdr::annotation::AnnotationType::Line;

                basegfx::B2DPolygon aPolygon;
                aPolygon.append(pMarker->maLineStart);
                aPolygon.append(pMarker->maLineEnd);
                aInfo.maPolygons.push_back(aPolygon);

                aInfo.mnWidth = pMarker->mnWidth;
                aInfo.maColor = rPDFAnnotation.maColor;
                aInfo.mbColor = true;
                aInfo.mbFillColor = false;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::FreeText)
            {
                aInfo.meType = sdr::annotation::AnnotationType::FreeText;
            }
            else if (rPDFAnnotation.meSubType == vcl::pdf::PDFAnnotationSubType::Stamp)
            {
                auto* pMarker = static_cast<vcl::pdf::PDFAnnotationMarkerStamp*>(
                    rPDFAnnotation.mpMarker.get());

                aInfo.meType = sdr::annotation::AnnotationType::Stamp;
                aInfo.maBitmap = pMarker->maBitmap;
            }

            xAnnotation->setCreationInfo(aInfo);
        }

        page.addAnnotation(xAnnotation, -1);
    }
    page.setLinkAnnotations(rPDFGraphicResult.GetLinksInfo());
}

static bool ImportPDF(SvStream& rStream, SdDrawDocument& rDocument,
                      std::unique_ptr<vcl::PDFMetadataReader>* deferred = nullptr)
{
    std::vector<vcl::PDFGraphicResult> aGraphics;
    if (vcl::ImportPDFUnloaded(rStream, aGraphics, deferred) == 0)
        return false;

    bool bWasLocked = rDocument.isLocked();
    rDocument.setLock(true);
    const bool bSavedUndoEnabled = rDocument.IsUndoEnabled();
    rDocument.EnableUndo(false);
    rDocument.setPDFDocument(true);

    SdrModel& rModel = rDocument;

    // Add as many pages as we need up-front.
    rDocument.CreateFirstPages();
    sal_uInt16 nPageToDuplicate = 0;
    for (size_t i = 0; i < aGraphics.size() - 1; ++i)
    {
        // duplicating the last page is cheaper than repeatedly duplicating the first one
        nPageToDuplicate = rDocument.DuplicatePage(nPageToDuplicate);
    }

    bool bAllPagesSameSize(false);
    Size aMasterSizeHMM;

    for (vcl::PDFGraphicResult const& rPDFGraphicResult : aGraphics)
    {
        const Graphic& rGraphic = rPDFGraphicResult.GetGraphic();
        const Size& aSizeHMM = rPDFGraphicResult.GetSize();

        const sal_Int32 nPageNumber = rGraphic.getPageNumber();
        assert(nPageNumber >= 0 && o3tl::make_unsigned(nPageNumber) < aGraphics.size());

        // Create the page and insert the Graphic.
        SdPage* pPage = rDocument.GetSdPage(nPageNumber, PageKind::Standard);
        if (!pPage) // failed to duplicate page, out of memory?
            return false;

        // Make the page size match the rendered image.
        pPage->SetSize(aSizeHMM);
        if (nPageNumber == 0)
        {
            aMasterSizeHMM = aSizeHMM;
            bAllPagesSameSize = true;
        }
        else
            bAllPagesSameSize &= aMasterSizeHMM == aSizeHMM;

        rtl::Reference<SdrGrafObj> pSdrGrafObj
            = new SdrGrafObj(rModel, rGraphic, tools::Rectangle(Point(), aSizeHMM));

        pSdrGrafObj->SetResizeProtect(true);
        pSdrGrafObj->SetMoveProtect(true);

        pPage->InsertObject(pSdrGrafObj.get());

        SdPdfFilter::ApplyPageMetadata(*pPage, rPDFGraphicResult);
    }

    if (SdPage* pPage = bAllPagesSameSize ? rDocument.GetSdPage(0, PageKind::Standard) : nullptr)
    {
        SdPage& rMasterPage = static_cast<SdPage&>(pPage->TRG_GetMasterPage());
        rMasterPage.SetSize(aMasterSizeHMM);
    }

    rDocument.setLock(bWasLocked);
    rDocument.EnableUndo(bSavedUndoEnabled);
    return true;
}

bool SdPdfFilter::Import()
{
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
    const auto started = std::chrono::steady_clock::now();
#endif
    const OUString aFileName(
        mrMedium.GetURLObject().GetMainURL(INetURLObject::DecodeMechanism::NONE));

    std::unique_ptr<SvStream> xStream(::utl::UcbStreamHelper::CreateStream(
        aFileName, StreamMode::READ | StreamMode::SHARE_DENYNONE));

    std::unique_ptr<vcl::PDFMetadataReader> metadata;
    bool defer = false;
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
    defer = !Application::IsHeadlessModeEnabled() && !comphelper::LibreOfficeKit::isActive();
#endif
    const bool imported = xStream && ImportPDF(*xStream, mrDocument, defer ? &metadata : nullptr);
#if HAVE_FEATURE_LOONGOFFICE_PDF_FAST
    SAL_INFO("pdffast", "native-import success=" << imported
        << " pages=" << mrDocument.GetSdPageCount(PageKind::Standard)
        << " total_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
    if (imported && defer)
    {
        if (!sd::PdfFastDocument::Attach(mrDocument))
            return false;
        mrDocument.GetPdfFastDocument()->SetMetadataReader(std::move(metadata));
    }
#endif
    return imported;
}

bool SdPdfFilter::Export() { return false; }

extern "C" SAL_DLLPUBLIC_EXPORT bool TestFODGExportPDF(SvStream& rStream)
{
    bool bResetEnvVar = false;
    if (getenv("LO_IMPORT_USE_PDFIUM") == nullptr)
    {
        bResetEnvVar = true;
        osl_setEnvironment(OUString("LO_IMPORT_USE_PDFIUM").pData, OUString("1").pData);
    }
    comphelper::ScopeGuard aPDFiumEnvVarGuard([&]() {
        if (bResetEnvVar)
            osl_clearEnvironment(OUString("LO_IMPORT_USE_PDFIUM").pData);
    });

    const uno::Reference<uno::XComponentContext>& xContext(
        comphelper::getProcessComponentContext());
    uno::Reference<css::frame::XModel2> xModel(
        xContext->getServiceManager()->createInstanceWithContext(
            u"com.sun.star.drawing.DrawingDocument"_ustr, xContext),
        uno::UNO_QUERY_THROW);

    uno::Reference<css::frame::XLoadable> xModelLoad(xModel, uno::UNO_QUERY_THROW);
    xModelLoad->initNew();

    SdXImpressDocument& rDrawDoc = dynamic_cast<SdXImpressDocument&>(*xModel);

    bool ret = ImportPDF(rStream, *rDrawDoc.GetDoc());

    if (ret)
    {
        utl::TempFileFast aTempFile;

        uno::Reference<lang::XMultiServiceFactory> xMultiServiceFactory(
            comphelper::getProcessServiceFactory());
        uno::Reference<uno::XInterface> xInterface(
            xMultiServiceFactory->createInstance(u"com.sun.star.comp.Writer.XmlFilterAdaptor"_ustr),
            uno::UNO_QUERY);

        css::uno::Sequence<OUString> aUserData{ u"com.sun.star.comp.filter.OdfFlatXml"_ustr,
                                                u""_ustr,
                                                u"com.sun.star.comp.Draw.XMLOasisImporter"_ustr,
                                                u"com.sun.star.comp.Draw.XMLOasisExporter"_ustr,
                                                u""_ustr,
                                                u""_ustr,
                                                u"true"_ustr };
        uno::Sequence<beans::PropertyValue> aAdaptorArgs(comphelper::InitPropertySequence({
            { "UserData", uno::Any(aUserData) },
        }));
        css::uno::Sequence<uno::Any> aOuterArgs{ uno::Any(aAdaptorArgs) };

        uno::Reference<lang::XInitialization> xInit(xInterface, uno::UNO_QUERY_THROW);
        xInit->initialize(aOuterArgs);

        uno::Reference<document::XFilter> xFODGFilter(xInterface, uno::UNO_QUERY);
        uno::Reference<document::XExporter> xExporter(xFODGFilter, uno::UNO_QUERY);
        xExporter->setSourceDocument(xModel);

        uno::Reference<io::XOutputStream> xOutputStream(
            new utl::OStreamWrapper(*aTempFile.GetStream(StreamMode::READWRITE)));

        uno::Sequence<beans::PropertyValue> aDescriptor(comphelper::InitPropertySequence(
            { { "FilterName", uno::Any(u"OpenDocument Drawing Flat XML"_ustr) },
              { "OutputStream", uno::Any(xOutputStream) },
              { "FilterOptions",
                uno::Any(
                    u"{\"DecomposePDF\":{\"type\":\"boolean\",\"value\":\"true\"}}"_ustr) } }));
        xFODGFilter->filter(aDescriptor);
    }

    css::uno::Reference<css::util::XCloseable> xClose(xModel, css::uno::UNO_QUERY);
    xClose->close(false);

    return ret;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
