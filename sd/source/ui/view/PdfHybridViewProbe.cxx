/* SPDX-License-Identifier: MPL-2.0 */

#include <DrawViewShell.hxx>
#include <PdfHybridViewProbe.hxx>
#include <Window.hxx>
#include <drawdoc.hxx>
#include <drawview.hxx>
#include <sdpage.hxx>

#include <comphelper/lok.hxx>
#include <drawinglayer/primitive2d/graphicprimitive2d.hxx>
#include <drawinglayer/primitive2d/PolyPolygonColorPrimitive2D.hxx>
#include <basegfx/polygon/b2dpolygontools.hxx>
#include <osl/file.hxx>
#include <rtl/string.hxx>
#include <svx/sdr/overlay/overlaymanager.hxx>
#include <svx/sdr/overlay/overlayprimitive2dsequenceobject.hxx>
#include <svx/sdrpaintwindow.hxx>
#include <svx/svdograf.hxx>
#include <tools/json_writer.hxx>

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace sd
{
namespace
{
bool enabled()
{
    const char* pFlag = std::getenv("LO_PDF_HYBRID_VIEW_PROBE");
    return pFlag && std::strcmp(pFlag, "1") == 0 && !comphelper::LibreOfficeKit::isActive();
}

// Deliberately restricted to a private three-page fixture. No global PDF open,
// save, slide show, master-page, printer or independent export-view override.
std::optional<pdfhybrid::Layout> getLayout(SdDrawDocument& rDoc, const SdPage* pActive)
{
    if (!enabled() || rDoc.GetDocumentType() != DocumentType::Draw
        || rDoc.GetSdPageCount(PageKind::Standard) != 3)
        return std::nullopt;
    constexpr OUString names[] = { u"P0A1 reading before"_ustr, u"P0A1 active edit"_ustr,
                                   u"P0A1 reading after"_ustr };
    std::array<pdfhybrid::Extent, 3> sizes;
    for (sal_uInt16 i = 0; i < 3; ++i)
    {
        const SdPage* pPage = rDoc.GetSdPage(i, PageKind::Standard);
        if (pPage->GetName() != names[i] || (i == 1 && pPage != pActive))
            return std::nullopt;
        if (i != 1 && (pPage->GetObjCount() != 1
                       || !dynamic_cast<const SdrGrafObj*>(pPage->GetObj(0))))
            return std::nullopt;
        sizes[i] = { pPage->GetSize().Width(), pPage->GetSize().Height() };
    }
    return pdfhybrid::makeLayout(sizes);
}

tools::Rectangle rectangle(const pdfhybrid::Rect& r)
{
    return tools::Rectangle(Point(r.x, r.y), Size(r.width, r.height));
}

void addPreview(PdfHybridViewProbe& rProbe, sdr::overlay::OverlayManager& rManager,
                const SdrGrafObj& rGraphic, const pdfhybrid::Rect& rRect)
{
    basegfx::B2DHomMatrix transform;
    transform.scale(rRect.width, rRect.height);
    transform.translate(rRect.x, rRect.y);
    const basegfx::B2DRange range(rRect.x, rRect.y, rRect.x + rRect.width,
                                 rRect.y + rRect.height);
    drawinglayer::primitive2d::Primitive2DContainer primitives;
    primitives.push_back(new drawinglayer::primitive2d::PolyPolygonColorPrimitive2D(
        basegfx::B2DPolyPolygon(basegfx::utils::createPolygonFromRect(range)),
        basegfx::BColor(1.0, 1.0, 1.0)));
    primitives.push_back(new drawinglayer::primitive2d::GraphicPrimitive2D(
        transform, rGraphic.GetGraphicObject()));
    auto overlay = std::make_unique<sdr::overlay::OverlayPrimitive2DSequenceObject>(
        std::move(primitives));
    overlay->setHittable(false);
    rManager.add(*overlay);
    rProbe.overlays.append(std::move(overlay));
}

void writeReady(const pdfhybrid::Layout& rLayout)
{
    const char* pPath = std::getenv("LO_PDF_HYBRID_VIEW_PROBE_REPORT");
    if (!pPath)
        return;
    OUString url;
    if (osl::FileBase::getFileURLFromSystemPath(OUString::fromUtf8(pPath), url)
        != osl::FileBase::E_None)
        return;
    osl::File file(url);
    // Create-only: never replace another file. The launcher supplies a unique
    // temporary path. This is an experiment handshake, not document metadata.
    if (file.open(osl_File_OpenFlag_Write | osl_File_OpenFlag_Create) != osl::FileBase::E_None)
        return;
    tools::JsonWriter json;
    json.put("status", std::string_view("host-attached"));
    json.put("overlays", 2);
    json.put("active_page_origin_x", 0);
    json.put("active_page_origin_y", 0);
    json.put("before_y", rLayout.pages[0].y);
    json.put("after_y", rLayout.pages[2].y);
    const OString data = json.finishAndGetAsOString();
    sal_uInt64 written = 0;
    file.write(data.getStr(), data.getLength(), written);
    file.close();
}
}

void DrawViewShell::UpdatePdfHybridViewProbe()
{
    // The common path makes no model queries when the experiment is disabled.
    if (!enabled() && !mpPdfHybridViewProbe)
        return;
    auto layout = mePageKind == PageKind::Standard && meEditMode == EditMode::Page
                      ? getLayout(*GetDoc(), mpActualPage) : std::nullopt;
    Window* pWindow = GetActiveWindow();
    if (!pWindow || !mpDrawView)
        return;
    SdrPaintWindow* pPaintWindow = layout ? mpDrawView->FindPaintWindow(*pWindow->GetOutDev()) : nullptr;
    rtl::Reference<sdr::overlay::OverlayManager> manager;
    if (pPaintWindow)
        manager = pPaintWindow->GetOverlayManager();
    if (mpPdfHybridViewProbe && layout && mpPdfHybridViewProbe->layout == *layout && manager
        && mpPdfHybridViewProbe->overlays.count() == 2
        && mpPdfHybridViewProbe->overlays.getOverlayObject(0).getOverlayManager() == manager.get()
        && pWindow->GetViewOrigin() == Point(-layout->workspace.x, -layout->workspace.y)
        && pWindow->GetViewSize() == Size(layout->workspace.width, layout->workspace.height)
        && mpDrawView->GetWorkArea() == rectangle(layout->pages[1]))
        return;

    const Point normalOrigin = mpPdfHybridViewProbe ? mpPdfHybridViewProbe->normalOrigin
                                                   : pWindow->GetViewOrigin();
    const Size normalSize = mpPdfHybridViewProbe ? mpPdfHybridViewProbe->normalSize
                                                : pWindow->GetViewSize();
    const tools::Rectangle normalArea = mpPdfHybridViewProbe ? mpPdfHybridViewProbe->normalWorkArea
                                                            : mpDrawView->GetWorkArea();
    const bool hadProbe = bool(mpPdfHybridViewProbe);
    mpPdfHybridViewProbe.reset(); // Detach overlays before changing view geometry.

    auto setWorkspace = [&](const Point& rOrigin, const Size& rSize,
                            const tools::Rectangle& rWorkArea) {
        // Preserve logical viewport position when changing the scroll extent.
        const Point position = pWindow->GetWinViewPos() + rOrigin - pWindow->GetViewOrigin();
        InitWindows(rOrigin, rSize, position, true);
        mpDrawView->SetWorkArea(rWorkArea);
        UpdateScrollBars();
    };
    if (!layout)
    {
        if (hadProbe)
            setWorkspace(normalOrigin, normalSize, normalArea);
        return;
    }
    if (!manager)
    {
        if (hadProbe)
            setWorkspace(normalOrigin, normalSize, normalArea);
        return;
    }
    auto probe = std::make_unique<PdfHybridViewProbe>(*layout, normalOrigin, normalSize, normalArea);
    const auto& work = layout->workspace;
    setWorkspace(Point(-work.x, -work.y), Size(work.width, work.height),
                 rectangle(layout->pages[1]));
    for (sal_uInt16 i : { 0, 2 })
        addPreview(*probe, *manager,
                   *static_cast<SdrGrafObj*>(GetDoc()->GetSdPage(i, PageKind::Standard)->GetObj(0)),
                   layout->pages[i]);
    mpPdfHybridViewProbe = std::move(probe);
    writeReady(*layout);
}

bool DrawViewShell::IsPdfHybridPreviewPoint(const Point& rPoint) const
{
    if (!mpPdfHybridViewProbe)
        return false;
    const auto& pages = mpPdfHybridViewProbe->layout.pages;
    return pages[0].contains(rPoint.X(), rPoint.Y()) || pages[2].contains(rPoint.X(), rPoint.Y());
}
}
