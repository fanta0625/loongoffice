/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <PdfFastDocument.hxx>
#include <DrawDocShell.hxx>
#include <drawdoc.hxx>
#include <sdpage.hxx>
#include <session.hxx>
#include <cache.hxx>
#include <transfer.hxx>
#include <basegfx/matrix/b2dhommatrixtools.hxx>
#include <drawinglayer/primitive2d/bitmapprimitive2d.hxx>
#include <comphelper/lok.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/propertysequence.hxx>
#include <com/sun/star/document/FilterFactory.hpp>
#include <com/sun/star/document/XFilter.hpp>
#include <com/sun/star/document/XImporter.hpp>
#include <com/sun/star/lang/XInitialization.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <sfx2/objsh.hxx>
#include <unotools/streamwrap.hxx>
#include <tools/stream.hxx>
#include <svx/sdr/contact/viewobjectcontact.hxx>
#include <svx/sdr/contact/viewcontact.hxx>
#include <svx/svdograf.hxx>
#include <svx/svdpage.hxx>
#include <svx/svdpagv.hxx>
#include <svx/svdview.hxx>
#include <svx/svditer.hxx>
#include <svx/svdlayer.hxx>
#include <svl/whiter.hxx>
#include <vcl/gfxlink.hxx>
#include <vcl/BitmapWriteAccess.hxx>
#include <vcl/dibtools.hxx>
#include <vcl/outdev.hxx>
#include <vcl/mapmod.hxx>
#include <vcl/svapp.hxx>
#include <vcl/timer.hxx>
#include <sal/log.hxx>
#include <comphelper/scopeguard.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

void SdDrawDocument::SetPdfFastDocument(std::unique_ptr<sd::PdfFastDocument> document)
{
    mpPdfFastDocument = std::move(document);
}

namespace sd
{
namespace
{
using pdffast::TileKey;
using Clock = std::chrono::steady_clock;
std::size_t cost(const TileKey& key)
{
    return std::size_t(key.width) * key.height * 4;
}
}

struct PdfFastDocument::Impl
{
    struct Page
    {
        rtl::Reference<SdrGrafObj> original;
        bool prepared = false;
        bool failed = false;
    };
    struct Viewport
    {
        std::vector<TileKey> visible;
        std::vector<TileKey> speculative;
        TileKey preview;
        Clock::time_point seen;
    };
    struct PaintState
    {
        MapMode mapMode;
        Size pixels;
        const SdrPage* page;
    };
    SdDrawDocument& doc;
    std::vector<Page> pages;
    std::unique_ptr<pdffast::Session> session;
    pdffast::Cache<TileKey, Bitmap> cache{ 128 * 1024 * 1024 };
    // Device addresses are identity only; never dereferenced or retained as windows.
    std::map<const OutputDevice*, Viewport> viewports;
    std::map<const OutputDevice*, PaintState> paintStates;
    Timer poll{ "Draw PDF tile delivery" };
    bool failed = false;
    bool preparing = false;

    explicit Impl(SdDrawDocument& document) : doc(document)
    {
        for (sal_uInt16 i = 0; i < doc.GetSdPageCount(PageKind::Standard); ++i)
        {
            auto* page = doc.GetSdPage(i, PageKind::Standard);
            rtl::Reference<SdrGrafObj> original;
            for (size_t n = 0; n < page->GetObjCount(); ++n)
            {
                auto* graphic = dynamic_cast<SdrGrafObj*>(page->GetObj(n));
                if (graphic && graphic->GetGraphic().GetSharedGfxLink()
                    && graphic->GetGraphic().GetSharedGfxLink()->GetType() == GfxLinkType::NativePdf)
                {
                    original = graphic;
                    break;
                }
            }
            pages.push_back({ original, false, false });
        }
        const auto link = pages.front().original->GetGraphic().GetSharedGfxLink();
        // Use the very same imported source as the native graphics, never reopen
        // a path that may have changed after import. Session pins the immutable
        // storage on this thread before its worker starts, with no source copy.
        const auto& bytes = link->getDataContainer();
        session = std::make_unique<pdffast::Session>(bytes);
        poll.SetTimeout(16);
        poll.SetInvokeHandler(LINK(this, Impl, Deliver));
        poll.Start();
    }

    ~Impl()
    {
        poll.Stop();
        session.reset(); // cancel and join before the model or original graphics die
    }

    rtl::Reference<SdrPage> importEditablePage(const Graphic& graphic)
    {
        using namespace css;
        const auto link = graphic.GetSharedGfxLink();
        if (!link || graphic.getPageNumber() < 0)
            throw std::runtime_error("Missing native PDF page");
        const auto bytes = link->getDataContainer().getDataShared();
        if (!bytes || bytes->empty())
            throw std::runtime_error("Missing native PDF source");

        auto* nativeShell = new DrawDocShell(SfxObjectCreateMode::EMBEDDED, false, DocumentType::Draw);
        SfxObjectShellLock shell(nativeShell);
        comphelper::ScopeGuard close([&] { nativeShell->DoClose(); });
        if (!nativeShell->DoInitNew())
            throw std::runtime_error("Cannot initialize native PDF import document");
        const auto context = comphelper::getProcessComponentContext();
        const auto factory = document::FilterFactory::create(context);
        uno::Sequence<beans::PropertyValue> definition;
        if (!(factory->getByName(u"draw_pdf_import"_ustr) >>= definition))
            throw std::runtime_error("Missing native Draw PDF filter definition");
        OUString service;
        for (const auto& property : definition)
            if (property.Name == "FilterService")
                property.Value >>= service;
        if (service.isEmpty())
            throw std::runtime_error("Missing native Draw PDF filter service");
        uno::Reference<document::XFilter> filter(
            context->getServiceManager()->createInstanceWithContext(service, context),
            uno::UNO_QUERY_THROW);
        uno::Reference<lang::XInitialization>(filter, uno::UNO_QUERY_THROW)->initialize(
            { uno::Any(definition) });
        uno::Reference<document::XImporter>(filter, uno::UNO_QUERY_THROW)->setTargetDocument(
            nativeShell->GetModel());

        // Invoke the default desktop filter directly. Loading through sfx would
        // route back into the PDFium read-only filter when PDF Fast is enabled.
        // The input owns its stream; the immutable source remains pinned until
        // both the filter and input have been destroyed.
        uno::Reference<io::XInputStream> input(new utl::OSeekableInputStreamWrapper(
            new SvMemoryStream(const_cast<sal_uInt8*>(bytes->data()), bytes->size(), StreamMode::READ),
            true));
        const auto started = Clock::now();
        const bool imported = filter->filter(comphelper::InitPropertySequence({
            { "URL", uno::Any(u"private:stream"_ustr) },
            { "InputStream", uno::Any(input) },
            { "FilterName", uno::Any(u"draw_pdf_import"_ustr) },
            { "FilterOptions", uno::Any(u"LoongOfficePdfPage="_ustr
                + OUString::number(graphic.getPageNumber() + 1)) },
        }));
        auto& nativeDoc = *nativeShell->GetDoc();
        const auto count = nativeDoc.GetSdPageCount(PageKind::Standard);
        SAL_INFO("pdffast", "native-edit-import backend=poppler pdf_page=" << graphic.getPageNumber()
            << " success=" << imported << " pages=" << count << " total_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        // Also reject a stale xpdfimport which silently ignores the private page
        // option: editing must never import an entire document behind the UI.
        if (!imported || count != 1)
            throw std::runtime_error("Native single-page PDF import failed");

        // PDF styles have no external semantic identity. Materialize them using
        // Draw's own style-to-hard-attributes implementation before crossing
        // models; otherwise a same-name destination style can change rendering.
        nativeDoc.BurnInStyleSheetAttributes();
        auto staged = nativeDoc.GetSdPage(0, PageKind::Standard)->CloneSdrPage(doc);
        if (!staged.is())
            throw std::runtime_error("Cannot stage native PDF page objects");
        // Never retain the temporary document through a master-page reference.
        if (staged->TRG_HasMasterPage())
            staged->TRG_ClearMasterPage();
        // Page properties also have a stylesheet parent, independent of the
        // objects. Flatten inherited fill items before closing the source shell.
        auto& pageProperties = staged->getSdrPageProperties();
        SfxItemSet pageAttributes(pageProperties.GetItemSet());
        pageAttributes.SetParent(nullptr);
        SfxWhichIter which(pageProperties.GetItemSet());
        for (sal_uInt16 id = which.FirstWhich(); id; id = which.NextWhich())
        {
            const SfxPoolItem* item = nullptr;
            if (which.GetItemState(true, &item) == SfxItemState::SET)
                pageAttributes.Put(*item);
        }
        pageProperties.SetStyleSheet(nullptr);
        pageProperties.PutItemSet(pageAttributes);

        // Native page cloning preserves geometry, groups and connector links.
        // Map layer IDs by name, without Paste()'s recentering or layer override.
        auto& targetLayers = doc.GetLayerAdmin();
        const auto& sourceLayers = nativeDoc.GetLayerAdmin();
        SdrObjListIter objects(staged.get(), SdrIterMode::DeepWithGroups);
        while (objects.IsMore())
        {
            auto* object = objects.Next();
            const auto* layer = sourceLayers.GetLayerPerID(object->GetLayer());
            if (!layer)
                throw std::runtime_error("Missing native PDF object layer");
            auto* target = targetLayers.GetLayer(layer->GetName());
            if (!target)
                target = targetLayers.NewLayer(layer->GetName());
            object->SetLayer(target->GetID());
        }
        return staged;
    }

    void schedule()
    {
        std::map<TileKey, std::size_t> visible, previews;
        std::vector<pdffast::Request> requests;
        std::set<TileKey> queued;
        const auto now = Clock::now();
        for (auto it = viewports.begin(); it != viewports.end();)
            if (now - it->second.seen > std::chrono::seconds(2))
                it = viewports.erase(it);
            else
                ++it;
        auto add = [&](const TileKey& key, bool speculative) {
            if (!cache.contains(key) && queued.insert(key).second)
                requests.push_back({ key.page, key, speculative });
        };
        for (const auto& entry : viewports)
        {
            const auto& viewport = entry.second;
            previews[viewport.preview] = cost(viewport.preview);
            visible[viewport.preview] = cost(viewport.preview);
            add(viewport.preview, false);
        }
        for (const auto& entry : viewports)
            for (const auto& key : entry.second.visible)
            {
                visible[key] = cost(key);
                add(key, false);
            }
        cache.protect(std::move(visible), std::move(previews));
        std::size_t spare = cache.spareForSpeculation();
        for (const auto& entry : viewports)
            for (const auto& key : entry.second.speculative)
                if (cost(key) <= spare && !cache.contains(key))
                {
                    spare -= cost(key);
                    add(key, true);
                }
        session->request(std::move(requests));
        if (!failed)
            poll.Start();
    }

    DECL_LINK(Deliver, Timer*, void);
};

IMPL_LINK_NOARG(PdfFastDocument::Impl, Deliver, Timer*, void)
{
    const auto start = Clock::now();
    std::set<int> changed;
    for (std::size_t items = 0;
         pdffast::TransferBudget::allows(items, Clock::now() - start); ++items)
    {
        auto result = session->takeResult();
        if (!result)
            break;
        if (result->kind == pdffast::Result::Kind::Tile)
        {
            if (result->page < 0 || std::size_t(result->page) >= pages.size()
                || pages[result->page].prepared)
                continue;
            const auto imageStart = Clock::now();
            Bitmap bitmap(Size(result->pixels.width, result->pixels.height),
                          vcl::PixelFormat::N24_BPP);
            bool allocated = false;
            {
                BitmapScopedWriteAccess access(bitmap);
                allocated = bool(access);
            }
            const auto allocatedAt = Clock::now();
            if (allocated && ReadRawDIB(bitmap, result->pixels.data.data(), ScanlineFormat::N24BitTcBgr,
                           result->pixels.height, result->pixels.stride))
            {
                const bool visible = std::any_of(viewports.begin(), viewports.end(),
                    [&](const auto& entry) {
                        return entry.second.preview == result->tile
                            || std::find(entry.second.visible.begin(), entry.second.visible.end(),
                                         result->tile) != entry.second.visible.end();
                    });
                cache.put(result->tile, std::move(bitmap), cost(result->tile), !visible);
            }
            else
                failed = true;
            SAL_INFO("pdffast", "native-ui-image page=" << result->page
                << " allocation_us=" << std::chrono::duration_cast<std::chrono::microseconds>(allocatedAt - imageStart).count()
                << " upload_cache_us=" << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - allocatedAt).count());
            changed.insert(result->page);
        }
        else if (result->kind == pdffast::Result::Kind::Error
                 || result->kind == pdffast::Result::Kind::Password)
        {
            failed = true;
            SAL_WARN("pdffast", "Native tiled preview unavailable: " << result->message);
            for (std::size_t i = 0; i < pages.size(); ++i)
                changed.insert(i);
            poll.Stop();
            break;
        }
    }
    if (failed)
    {
        poll.Stop();
        for (std::size_t page = 0; page < pages.size(); ++page)
            changed.insert(page);
    }
    for (int page : changed)
        if (pages[page].original.is())
            pages[page].original->ActionChanged();
    // Do not resubmit missing tiles here: an over-budget view deliberately keeps
    // its preview instead of endlessly rerendering mutually evicted sharp tiles.
    if (!failed && !session->idle())
        poll.Start();
}

PdfFastDocument::PdfFastDocument(SdDrawDocument& doc) : m_impl(std::make_unique<Impl>(doc)) {}
PdfFastDocument::~PdfFastDocument() = default;

bool PdfFastDocument::Attach(SdDrawDocument& doc)
{
    if (comphelper::LibreOfficeKit::isActive() || !doc.GetSdPageCount(PageKind::Standard))
        return false;
    // Only accept the untouched native PDF import representation.
    auto* page = doc.GetSdPage(0, PageKind::Standard);
    if (!page->GetObjCount())
        return false;
    auto* object = dynamic_cast<SdrGrafObj*>(page->GetObj(0));
    if (!object || !object->GetGraphic().GetSharedGfxLink()
        || object->GetGraphic().GetSharedGfxLink()->GetType() != GfxLinkType::NativePdf)
        return false;
    doc.SetPdfFastDocument(std::unique_ptr<PdfFastDocument>(new PdfFastDocument(doc)));
    SAL_INFO("pdffast", "native-attach pages=" << doc.GetSdPageCount(PageKind::Standard));
    return true;
}

void PdfFastDocument::PreparePaint(SdrView& view, OutputDevice& output)
{
    auto& impl = *m_impl;
    const auto* pageView = view.GetSdrPageView();
    if (!pageView || impl.failed)
        return;
    const auto* page = pageView->GetPage();
    const Impl::PaintState state{ output.GetMapMode(), output.GetOutputSizePixel(), page };
    const auto previous = impl.paintStates.find(&output);
    if (previous != impl.paintStates.end() && previous->second.mapMode == state.mapMode
        && previous->second.pixels == state.pixels && previous->second.page == state.page)
        return;
    impl.paintStates.insert_or_assign(&output, state);
    // Draw caches the redirected primitive sequence. A viewport/zoom change
    // alone does not invalidate object geometry, so Paint() may never run to
    // request newly visible tiles. Invalidate before primitive traversal, not
    // inside the redirector, and only when the viewport actually changes.
    for (const auto& entry : impl.pages)
        if (!entry.prepared && entry.original.is()
            && entry.original->getSdrPageFromSdrObject() == page)
            entry.original->ActionChanged();
    SAL_INFO("pdffast", "native-viewport pixels=" << state.pixels.Width()
        << 'x' << state.pixels.Height());
}

bool PdfFastDocument::Paint(const sdr::contact::ViewObjectContact& contact, OutputDevice& output,
    drawinglayer::primitive2d::Primitive2DDecompositionVisitor& visitor)
{
    auto& impl = *m_impl;
    if (impl.failed)
        return false;
    auto* object = contact.GetViewContact().TryToGetSdrObject();
    auto it = std::find_if(impl.pages.begin(), impl.pages.end(),
        [&](const Impl::Page& page) { return page.original.get() == object && !page.prepared; });
    if (it == impl.pages.end())
        return false;
    const int page = std::distance(impl.pages.begin(), it);
    const auto rect = it->original->GetLogicRect();
    if (rect.IsEmpty() || it->original->GetRotateAngle() != Degree100(0)
        || it->original->GetShearAngle() != Degree100(0))
        return false;
    basegfx::B2DRange visible(rect.Left(), rect.Top(), rect.Right(), rect.Bottom());
    // Demand follows the complete window, not ViewInformation2D's paint clip.
    // A small invalidation (e.g. a delivered tile) must not cancel the rest of
    // the visible work. It also bounds requests if the paint viewport is empty.
    const auto window = output.PixelToLogic(
        tools::Rectangle(Point(), output.GetOutputSizePixel()));
    visible.intersect(basegfx::B2DRange(window.Left(), window.Top(), window.Right(), window.Bottom()));
    if (visible.isEmpty())
        return true;
    const Size pixels = output.LogicToPixel(rect.GetSize());
    const int width = std::clamp<tools::Long>(pixels.Width(), 1, 1000000);
    const int height = std::clamp<tools::Long>(pixels.Height(), 1, 1000000);
    // A zoom-independent preview survives every zoom/viewport change.
    const double previewScale = std::min(256.0 / rect.GetWidth(), 512.0 / rect.GetHeight());
    const int pw = std::max(1, int(rect.GetWidth() * previewScale));
    const int ph = std::max(1, int(rect.GetHeight() * previewScale));
    Impl::Viewport viewport;
    viewport.preview = { page, pw, ph, 0, 0, pw, ph };
    viewport.seen = Clock::now();
    const double sx = double(width) / rect.GetWidth();
    const double sy = double(height) / rect.GetHeight();
    const int left = std::clamp(int((visible.getMinX() - rect.Left()) * sx), 0, width - 1) / 512 * 512;
    const int top = std::clamp(int((visible.getMinY() - rect.Top()) * sy), 0, height - 1) / 512 * 512;
    const int right = std::clamp(int(std::ceil((visible.getMaxX() - rect.Left()) * sx)), 1, width);
    const int bottom = std::clamp(int(std::ceil((visible.getMaxY() - rect.Top()) * sy)), 1, height);
    for (int y = top; y < bottom; y += 512)
        for (int x = left; x < right; x += 512)
            viewport.visible.push_back({ page, width, height, x, y,
                                        std::min(512, width - x), std::min(512, height - y) });
    // One row beyond the viewport, budgeted behind every visible request.
    const int next = (bottom + 511) / 512 * 512;
    if (next < height)
        for (int x = left; x < right; x += 512)
            viewport.speculative.push_back({ page, width, height, x, next,
                                            std::min(512, width - x), std::min(512, height - next) });
    const auto previous = impl.viewports.find(&output);
    const bool changed = previous == impl.viewports.end()
        || previous->second.visible != viewport.visible || !(previous->second.preview == viewport.preview);
    impl.viewports[&output] = viewport;
    if (changed)
        impl.schedule();

    auto draw = [&](const TileKey& key) {
        if (const Bitmap* bitmap = impl.cache.get(key))
        {
            const double dx = double(rect.GetWidth()) / key.pageWidth;
            const double dy = double(rect.GetHeight()) / key.pageHeight;
            visitor.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::BitmapPrimitive2D(*bitmap,
                    basegfx::utils::createScaleTranslateB2DHomMatrix(
                        key.width * dx, key.height * dy,
                        rect.Left() + key.x * dx, rect.Top() + key.y * dy))));
        }
    };
    draw(viewport.preview);
    std::vector<TileKey> gaps;
    for (const auto& key : viewport.visible)
        if (!impl.cache.contains(key))
            gaps.push_back(key);
    if (!gaps.empty())
        for (const auto& old : impl.cache.keys([&](const TileKey& key) {
            return key.page == page && (key.pageWidth != width || key.pageHeight != height)
                && !(key == viewport.preview)
                && std::any_of(gaps.begin(), gaps.end(), [&](const TileKey& gap) {
                       return pdffast::fallbackIntersects(key, gap);
                   });
        }))
            draw(old);
    for (const auto& key : viewport.visible)
        draw(key);
    return true;
}

void PdfFastDocument::PrepareCurrentPage(SdrView& view)
{
    auto& impl = *m_impl;
    if (impl.preparing)
        return;
    const auto* pageView = view.GetSdrPageView();
    if (!pageView || pageView->GetPage()->IsMasterPage())
        return;
    auto* page = pageView->GetPage();
    auto it = std::find_if(impl.pages.begin(), impl.pages.end(), [&](const Impl::Page& p) {
        return !p.prepared && !p.failed && p.original.is()
            && p.original->getSdrPageFromSdrObject() == page;
    });
    if (it == impl.pages.end())
        return;
    impl.preparing = true;
    comphelper::ScopeGuard preparing([&] { impl.preparing = false; });
    const auto started = Clock::now();
    const bool changed = impl.doc.IsChanged();
    const bool undo = impl.doc.IsUndoEnabled();
    const auto originalRect = it->original->GetLogicRect();
    SAL_INFO("pdffast", "native-edit-begin page=" << std::distance(impl.pages.begin(), it)
        << " pdf_page=" << it->original->GetGraphic().getPageNumber()
        << " x=" << originalRect.Left() << " y=" << originalRect.Top()
        << " width=" << originalRect.GetWidth() << " height=" << originalRect.GetHeight());
    // Other windows and pages retain their own raster demand during import.
    for (auto viewport = impl.viewports.begin(); viewport != impl.viewports.end();)
        if (viewport->second.preview.page == std::distance(impl.pages.begin(), it))
            viewport = impl.viewports.erase(viewport);
        else
            ++viewport;
    impl.schedule();
    comphelper::ScopeGuard restore([&] {
        view.UnmarkAllObj();
        impl.doc.EnableUndo(undo);
        impl.doc.SetChanged(changed);
    });
    impl.doc.EnableUndo(false);
    view.UnmarkAllObj();
    try
    {
        auto staged = impl.importEditablePage(it->original->GetGraphic());
        auto* stagedPage = dynamic_cast<SdPage*>(staged.get());
        auto* targetPage = dynamic_cast<SdPage*>(page);
        if (!stagedPage || !targetPage)
            throw std::runtime_error("Missing native Draw page");
        const auto originalPosition = it->original->GetOrdNum();
        std::vector<rtl::Reference<SdrObject>> objects;
        objects.reserve(staged->GetObjCount());
        while (staged->GetObjCount())
            objects.push_back(staged->NbcRemoveObject(0));
        bool committed = false;
        comphelper::ScopeGuard rollback([&] {
            if (committed)
                return;
            for (const auto& object : objects)
                if (object->getSdrPageFromSdrObject() == page)
                    page->NbcRemoveObject(object->GetOrdNum());
            if (it->original->getSdrPageFromSdrObject() != page)
                page->NbcInsertObject(it->original.get(), originalPosition);
            page->GetViewContact().ActionChanged();
        });
        std::size_t position = originalPosition;
        for (const auto& object : objects)
            page->InsertObject(object.get(), position++);
        // SdPage cloning rebinds presentation callbacks to the staging page.
        // Moving its objects must not leave that temporary page as a callback.
        SdrObjListIter callbacks(page, SdrIterMode::DeepWithGroups);
        while (callbacks.IsMore())
        {
            auto* object = callbacks.Next();
            if (object->GetUserCall() == stagedPage)
                object->SetUserCall(targetPage);
        }
        page->RemoveObject(it->original->GetOrdNum());
        committed = true;
        it->prepared = true; // A blank native page is a valid editable page too.
        page->SetSize(staged->GetSize());
        page->SetBorder(staged->GetLeftBorder(), staged->GetUpperBorder(),
            staged->GetRightBorder(), staged->GetLowerBorder());
        auto& pageProperties = page->getSdrPageProperties();
        pageProperties.SetStyleSheet(nullptr);
        pageProperties.ClearItem();
        pageProperties.PutItemSet(staged->getSdrPageProperties().GetItemSet());
        page->GetViewContact().ActionChanged();
    }
    catch (const css::uno::Exception& error)
    {
        SAL_WARN("pdffast", "Native single-page import failed: " << error.Message);
    }
    catch (const std::exception& error)
    {
        SAL_WARN("pdffast", "Native single-page import failed: " << error.what());
    }
    it->failed = !it->prepared;
    SAL_INFO("pdffast", "native-edit-prepare page=" << std::distance(impl.pages.begin(), it)
        << " success=" << it->prepared << " objects=" << page->GetObjCount() << " total_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
}
}
