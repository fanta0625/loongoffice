/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <PdfFastDocument.hxx>
#include <DrawDocShell.hxx>
#include <drawdoc.hxx>
#include <sdpage.hxx>
#include <sdpdffilter.hxx>
#include <vcl/pdfread.hxx>
#include <vcl/idle.hxx>
#include <session.hxx>
#include <cache.hxx>
#include <transfer.hxx>
#include <viewpolicy.hxx>
#include <nativeimport.hxx>
#include <basegfx/matrix/b2dhommatrixtools.hxx>
#include <basegfx/polygon/b2dpolygontools.hxx>
#include <basegfx/polygon/b2dpolypolygon.hxx>
#include <drawinglayer/primitive2d/bitmapprimitive2d.hxx>
#include <drawinglayer/primitive2d/maskprimitive2d.hxx>
#include <comphelper/lok.hxx>
#include <comphelper/processfactory.hxx>
#include <comphelper/propertysequence.hxx>
#include <com/sun/star/container/XNameAccess.hpp>
#include <com/sun/star/document/XFilter.hpp>
#include <com/sun/star/document/XImporter.hpp>
#include <com/sun/star/lang/XInitialization.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <sfx2/objsh.hxx>
#include <svx/sdr/contact/viewobjectcontact.hxx>
#include <svx/sdr/contact/viewcontact.hxx>
#include <svx/svdograf.hxx>
#include <svx/svdpage.hxx>
#include <svx/svdpagv.hxx>
#include <svx/svdview.hxx>
#include <svx/svditer.hxx>
#include <svx/svdlayer.hxx>
#include <svx/annotation/Annotation.hxx>
#include <svl/whiter.hxx>
#include <svx/svdmodel.hxx>
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
#include <deque>
#include <cmath>
#include <map>
#include <limits>
#include <optional>
#include <mutex>
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
    const auto padded = pdffast::tileWithGutter(key);
    return std::size_t(padded.width) * padded.height * 4;
}
}

struct PdfFastDocument::Impl
{
    struct Page
    {
        rtl::Reference<SdrGrafObj> original;
        bool prepared = false;
        bool failed = false;
        bool metadataReady = true;
        bool metadataFailed = false;
    };
    struct Viewport
    {
        std::vector<TileKey> visible;
        std::vector<TileKey> speculative;
        std::vector<TileKey> adjacent;
        std::vector<TileKey> adjacentSharp;
        TileKey preview;
        Clock::time_point seen;
        pdffast::ZoomSettle zoom;
        bool deferred = false;
        bool native = false;
    };
    struct PaintState
    {
        MapMode mapMode;
        Size pixels;
        const SdrPage* page;
    };
    SdDrawDocument& doc;
    std::deque<Page> pages;
    std::map<const SdrObject*, Page*> rawPages;
    std::unique_ptr<pdffast::Session> session;
    pdffast::Cache<TileKey, Bitmap> cache{ 128 * 1024 * 1024 };
    // Device addresses are identity only; never dereferenced or retained as windows.
    std::map<const OutputDevice*, Viewport> viewports;
    std::map<const OutputDevice*, PaintState> paintStates;
    std::map<const OutputDevice*, const SdrView*> devices;
    struct Thumbnail
    {
        Clock::time_point seen;
        bool visible;
    };
    std::map<TileKey, Thumbnail> thumbnails;
    std::shared_ptr<const std::vector<sal_uInt8>> source;
    std::unique_ptr<pdffast::NativeImport> importer;
    struct EditIntent
    {
        rtl::Reference<SdrGrafObj> original;
        Clock::time_point due;
    };
    std::map<SdrView*, EditIntent> intents;
    struct EditGate
    {
        PdfFastDocument::EditState state = PdfFastDocument::EditState::Ready;
        Link<bool, void> changed;
    };
    std::map<SdrView*, EditGate> gates;
    void setGate(SdrView& view, PdfFastDocument::EditState state)
    {
        auto& gate = gates[&view];
        gate.state = state;
        const bool blocked = state != PdfFastDocument::EditState::Ready;
        if (auto* page = view.GetSdrPageView())
            page->SetContentReadOnly(blocked);
        if (blocked)
            view.UnmarkAllObj();
        gate.changed.Call(blocked);
    }
    rtl::Reference<SdrGrafObj> running;
    int runningPage = -1;
    std::optional<pdffast::NativeImport::Result> pendingEditResult;
    Clock::time_point lastScroll{};
    Timer poll{ "Draw PDF tile delivery" };
    void wake()
    {
        // Sidebar retries run every 10 ms. Restarting a 16 ms timer on every
        // request would postpone delivery forever while thumbnails are pending.
        if (!poll.IsActive())
            poll.Start();
    }
    Idle metadataIdle{ "Draw PDF deferred metadata" };
    std::mutex editEventMutex;
    ImplSVEvent* editEvent = nullptr;
    DECL_LINK(EditCompleted, void*, void);
    std::unique_ptr<vcl::PDFMetadataReader> metadata;
    bool firstContent = false;
    bool failed = false;
    bool preparing = false;
    bool editMode = false;

    bool editPending(int page) const
    {
        return std::any_of(intents.begin(), intents.end(), [&](const auto& intent) {
            return intent.second.original.is()
                   && intent.second.original->GetGraphic().getPageNumber() == page;
        });
    }

    void setEditMode(bool enabled)
    {
        if (editMode == enabled)
            return;
        editMode = enabled;
        if (editMode)
            return;

        // Leaving the edit mode invalidates every pending page intent. Let the
        // normal completion path consume and release a cancelled helper result
        // so a later explicit edit can reuse the session safely.
        intents.clear();
        for (auto & [ view, gate ] : gates)
            if (gate.state != PdfFastDocument::EditState::Ready)
                setGate(*view, PdfFastDocument::EditState::Ready);
        if (running.is())
            importer->cancel();
        wake();
    }

    explicit Impl(SdDrawDocument& document)
        : doc(document)
    {
        for (sal_uInt16 i = 0; i < doc.GetSdPageCount(PageKind::Standard); ++i)
        {
            auto* page = doc.GetSdPage(i, PageKind::Standard);
            rtl::Reference<SdrGrafObj> original;
            for (size_t n = 0; n < page->GetObjCount(); ++n)
            {
                auto* graphic = dynamic_cast<SdrGrafObj*>(page->GetObj(n));
                if (graphic && graphic->GetGraphic().GetSharedGfxLink()
                    && graphic->GetGraphic().GetSharedGfxLink()->GetType()
                           == GfxLinkType::NativePdf)
                {
                    original = graphic;
                    break;
                }
            }
            pages.push_back({ original, false, false });
            rawPages.emplace(original.get(), &pages.back());
        }
        const auto link = pages.front().original->GetGraphic().GetSharedGfxLink();
        // Use the very same imported source as the native graphics, never reopen
        // a path that may have changed after import. Session pins the immutable
        // storage on this thread before its worker starts, with no source copy.
        const auto& bytes = link->getDataContainer();
        source = bytes.getDataShared();
        session = std::make_unique<pdffast::Session>(bytes);
        // Create the default event window on the UI thread before workers use it.
        Application::GetDefaultDevice();
        importer = std::make_unique<pdffast::NativeImport>(source, std::chrono::minutes(2), [this] {
            std::scoped_lock lock(editEventMutex);
            if (!editEvent)
                editEvent = Application::PostUserEvent(LINK(this, Impl, EditCompleted));
        });
        poll.SetTimeout(16);
        poll.SetInvokeHandler(LINK(this, Impl, Deliver));
        metadataIdle.SetPriority(TaskPriority::LOWEST);
        metadataIdle.SetInvokeHandler(LINK(this, Impl, ReadMetadata));
        wake();
    }

    // A cloned/reordered/undo-restored raw page still refers to the same frozen
    // PDF. Discover by object identity, but address raster work by PDF page index.
    Page* findPage(SdrPage& page)
    {
        for (size_t n = 0; n < page.GetObjCount(); ++n)
        {
            auto* object = dynamic_cast<SdrGrafObj*>(page.GetObj(n));
            if (!object)
                continue;
            const auto& graphic = object->GetGraphic();
            const auto link = graphic.GetSharedGfxLink();
            if (!link || link->GetType() != GfxLinkType::NativePdf
                || link->getDataContainer().getDataShared() != source)
                continue;
            if (const auto found = rawPages.find(object); found != rawPages.end())
                return found->second;
            pages.push_back({ object, false, false });
            rawPages.emplace(object, &pages.back());
            return &pages.back();
        }
        return nullptr;
    }

    bool preparePage(SdrPage&, SdrView*, const pdffast::NativeImport::Result* = nullptr);
    bool processEdits();
    bool ensureMetadata(SdrPage& target)
    {
        auto* entry = findPage(target);
        if (!entry || entry->metadataReady)
            return true;
        if (!metadata)
            return false;
        const auto started = Clock::now();
        entry->metadataFailed = true;
        try
        {
            auto result = metadata->ReadPage(entry->original->GetGraphic().getPageNumber());
            if (!result)
                return false;
            const bool changed = doc.IsChanged();
            const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
            const bool undo = doc.IsUndoEnabled();
            const bool locked = doc.isLocked();
            doc.EnableUndo(false);
            doc.setLock(true);
            comphelper::ScopeGuard restore([&] {
                doc.setLock(locked);
                doc.EnableUndo(undo);
                doc.SetChanged(changed);
                if (auto* shell = doc.GetDocSh())
                    shell->SetModified(modified);
            });
            // A malformed annotation must not leave a half-applied page that would
            // duplicate annotations when the user retries.
            auto& page = static_cast<SdPage&>(target);
            const auto annotations = page.getAnnotations();
            const auto links = page.getLinkAnnotations();
            bool applied = false;
            comphelper::ScopeGuard rollback([&] {
                if (applied)
                    return;
                const auto partial = page.getAnnotations();
                for (const auto& annotation : partial)
                    if (std::find(annotations.begin(), annotations.end(), annotation)
                        == annotations.end())
                        page.removeAnnotationNoNotify(annotation);
                page.setLinkAnnotations(links);
            });
            SdPdfFilter::ApplyPageMetadata(page, *result);
            applied = true;
            entry->metadataReady = true;
            entry->metadataFailed = false;
            target.ActionChanged();
            SAL_INFO("pdffast", "native-metadata page="
                                    << result->GetGraphic().getPageNumber()
                                    << " annotations=" << result->GetAnnotations().size()
                                    << " links=" << result->GetLinksInfo().size() << " total_us="
                                    << std::chrono::duration_cast<std::chrono::microseconds>(
                                           Clock::now() - started)
                                           .count());
            return true;
        }
        catch (const css::uno::Exception& error)
        {
            SAL_WARN("pdffast", "Deferred PDF metadata failed: " << error.Message);
        }
        catch (const std::exception& error)
        {
            SAL_WARN("pdffast", "Deferred PDF metadata failed: " << error.what());
        }
        return false;
    }

    ~Impl()
    {
        poll.Stop();
        metadataIdle.Stop();
        importer.reset(); // join the producer before removing its queued UI event
        if (editEvent)
            Application::RemoveUserEvent(editEvent);
        session.reset(); // cancel and join before the model or original graphics die
    }

    rtl::Reference<SdrPage> importEditablePage(const Graphic& graphic,
                                               const pdffast::NativeImport::Result& result)
    {
        using namespace css;
        const auto link = graphic.GetSharedGfxLink();
        if (!link || graphic.getPageNumber() < 0)
            throw std::runtime_error("Missing native PDF page");
        const auto bytes = link->getDataContainer().getDataShared();
        if (!bytes || bytes->empty())
            throw std::runtime_error("Missing native PDF source");
        if (!result.success || result.page != graphic.getPageNumber())
            throw std::runtime_error("Native PDF transport failed");

        // Initialize an import/data model. Completing a new Draw document here
        // installs defaults such as automatic kerning which the normal PDF
        // loading path does not use, changing the imported text layout.
        auto* nativeShell
            = new DrawDocShell(SfxObjectCreateMode::EMBEDDED, true, DocumentType::Draw);
        SfxObjectShellLock shell(nativeShell);
        comphelper::ScopeGuard close([&] { nativeShell->DoClose(); });
        if (!nativeShell->DoInitNew())
            throw std::runtime_error("Cannot initialize native PDF import document");
        const auto context = comphelper::getProcessComponentContext();
        uno::Reference<container::XNameAccess> factory(
            context->getServiceManager()->createInstanceWithContext(
                u"com.sun.star.document.FilterFactory"_ustr, context),
            uno::UNO_QUERY_THROW);
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
        uno::Reference<lang::XInitialization>(filter, uno::UNO_QUERY_THROW)
            ->initialize({ uno::Any(definition) });
        uno::Reference<document::XImporter>(filter, uno::UNO_QUERY_THROW)
            ->setTargetDocument(nativeShell->GetModel());

        // Invoke the default desktop filter directly. Loading through sfx would
        // route back into the PDFium read-only filter when PDF Fast is enabled.
        // Replay only a completed helper job. Files, helper startup/parse and
        // pipe waits happened on the transport worker; UNO/VCL stay on this thread.
        const auto started = Clock::now();
        const bool imported = filter->filter(comphelper::InitPropertySequence({
            { "URL", uno::Any(u"private:stream"_ustr) },
            { "LoongOfficePdfCommands", uno::Any(result.commands) },
            { "LoongOfficePdfBinary", uno::Any(result.binary) },
            { "FilterName", uno::Any(u"draw_pdf_import"_ustr) },
            { "FilterOptions", uno::Any(u"LoongOfficePdfPage="_ustr
                                        + OUString::number(graphic.getPageNumber() + 1)) },
        }));
        auto& nativeDoc = *nativeShell->GetDoc();
        const auto count = nativeDoc.GetSdPageCount(PageKind::Standard);
        SAL_INFO("pdffast", "native-edit-import backend=poppler pdf_page="
                                << graphic.getPageNumber() << " success=" << imported
                                << " pages=" << count << " total_us="
                                << std::chrono::duration_cast<std::chrono::microseconds>(
                                       Clock::now() - started)
                                       .count());
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

    void collectAdjacent(const SdrPage& current, OutputDevice& output, Viewport& viewport)
    {
        const auto number = current.GetPageNum();
        const int index = (int(number) - 1) / 2;
        const int count = doc.GetSdPageCount(PageKind::Standard);
        if (number == 0 || index < 0 || index >= count
            || doc.GetSdPage(index, PageKind::Standard) != &current)
            return;
        const auto window
            = output.PixelToLogic(tools::Rectangle(Point(), output.GetOutputSizePixel()));
        for (const int adjacent : { index + 1, index - 1 })
        {
            if (adjacent < 0 || adjacent >= count)
                continue;
            const auto* neighbor = doc.GetSdPage(adjacent, PageKind::Standard);
            if (neighbor->GetObjCount() != 1)
                continue;
            const auto known = rawPages.find(neighbor->GetObj(0));
            if (known == rawPages.end() || known->second->prepared || !known->second->original.is())
                continue;
            const auto& original = known->second->original;
            const auto bounds = original->GetLogicRect();
            if (bounds.IsEmpty() || original->GetRotateAngle() != Degree100(0)
                || original->GetShearAngle() != Degree100(0))
                continue;
            const int page = original->GetGraphic().getPageNumber();
            viewport.adjacent.push_back(
                pdffast::pagePreview(page, bounds.GetWidth(), bounds.GetHeight()));
            const Size pixels = output.LogicToPixel(bounds.GetSize());
            const int width = std::clamp<tools::Long>(pixels.Width(), 1, 1000000);
            const int height = std::clamp<tools::Long>(pixels.Height(), 1, 1000000);
            const double sx = double(width) / bounds.GetWidth();
            const int left = std::clamp(int((window.Left() - bounds.Left()) * sx), 0, width - 1);
            const int right = std::clamp(int(std::ceil((window.Right() - bounds.Left()) * sx)),
                                         left + 1, width);
            const int firstX = left / 512 * 512;
            const int firstY = adjacent > index ? 0 : (height - 1) / 512 * 512;
            const int step = adjacent > index ? 512 : -512;
            int added = 0;
            for (int y = firstY; y >= 0 && y < height && added < 8; y += step)
            {
                if (std::abs(y - firstY) >= 1024)
                    break;
                for (int x = firstX; x < right && added < 8; x += 512, ++added)
                    viewport.adjacentSharp.push_back({ page, width, height, x, y,
                                                       std::min(512, width - x),
                                                       std::min(512, height - y) });
            }
        }
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
        for (auto it = thumbnails.begin(); it != thumbnails.end();)
            if (now - it->second.seen > std::chrono::seconds(2))
                it = thumbnails.erase(it);
            else
                ++it;
        auto add = [&](const TileKey& key, bool speculative) {
            if (!cache.contains(key) && queued.insert(key).second)
                requests.push_back({ key.page, key, speculative });
        };
        for (const auto& entry : viewports)
        {
            const auto& viewport = entry.second;
            if (viewport.native)
                continue;
            previews[viewport.preview] = cost(viewport.preview);
            visible[viewport.preview] = cost(viewport.preview);
            add(viewport.preview, false);
        }
        // Visible navigation previews must not wait for a full main-view
        // refinement. Non-visible thumbnails remain background work.
        for (const auto& entry : thumbnails)
            if (entry.second.visible)
            {
                previews[entry.first] = cost(entry.first);
                visible[entry.first] = cost(entry.first);
                add(entry.first, false);
            }
        for (const auto& entry : viewports)
        {
            if (entry.second.native)
                continue;
            // Page navigation is preview-first. Do not enqueue all visible
            // high-resolution tiles until the new page's bounded preview has
            // arrived; on a complex page those tiles can otherwise delay the
            // first useful frame by several seconds on slower CPUs.
            if (entry.second.zoom.pending(now) || !cache.contains(entry.second.preview)
                || editPending(entry.second.preview.page))
                continue;
            for (const auto& key : entry.second.visible)
            {
                visible[key] = cost(key);
                // Foreground previews can preempt a running sharp region.
                add(key, true);
            }
        }
        // Background thumbnails cannot preempt visible content.
        for (const auto& entry : thumbnails)
            if (!entry.second.visible)
                add(entry.first, true);
        cache.protect(std::move(visible), std::move(previews));
        std::size_t spare = cache.spareForSpeculation();
        if (!preparing && !running.is() && intents.empty())
            for (const auto& entry : viewports)
                for (const auto& key : entry.second.adjacent)
                    if (cost(key) <= spare && !cache.contains(key))
                    {
                        spare -= cost(key);
                        add(key, true);
                    }
        for (const auto& entry : viewports)
        {
            if (entry.second.native)
                continue;
            if (entry.second.zoom.pending(now) || editPending(entry.second.preview.page))
                continue;
            for (const auto& key : entry.second.speculative)
                if (cost(key) <= spare && !cache.contains(key))
                {
                    spare -= cost(key);
                    add(key, true);
                }
        }
        if (!preparing && !running.is() && intents.empty())
            for (const auto& entry : viewports)
                if (!entry.second.zoom.pending(now))
                    for (const auto& key : entry.second.adjacentSharp)
                        if (cost(key) <= spare && !cache.contains(key))
                        {
                            spare -= cost(key);
                            add(key, true);
                        }
        session->request(std::move(requests));
        if (!failed)
            wake();
    }

    DECL_LINK(Deliver, Timer*, void);
    DECL_LINK(ReadMetadata, Timer*, void);
};

IMPL_LINK_NOARG(PdfFastDocument::Impl, ReadMetadata, Timer*, void)
{
    if (preparing || running.is() || !intents.empty() || !session->idle())
        return; // Delivery restarts the idle after interactive work.
    // Only visible pages get their links and annotations during interaction.
    for (const auto & [ device, view ] : devices)
        if (auto* pageView = view->GetSdrPageView())
            if (auto* entry = findPage(*pageView->GetPage());
                entry && !entry->metadataReady && !entry->metadataFailed)
            {
                ensureMetadata(*pageView->GetPage());
                metadataIdle.Start();
                return;
            }
    // Do not scan an entire large PDF in UI idles. A metadata read takes the
    // process-wide PDFium lock and can otherwise wait behind raster work.
    // Unvisited pages are completed by the existing save/export barriers.
}

IMPL_LINK_NOARG(PdfFastDocument::Impl, Deliver, Timer*, void)
{
    const bool editsPending = processEdits();
    const auto start = Clock::now();
    std::set<int> changed;
    bool previewDelivered = false;
    for (std::size_t items = 0; pdffast::TransferBudget::allows(items, Clock::now() - start);
         ++items)
    {
        auto result = session->takeResult();
        if (!result)
            break;
        if (result->kind == pdffast::Result::Kind::Tile)
        {
            if (result->page < 0)
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
            if (allocated
                && ReadRawDIB(bitmap, result->pixels.data.data(), ScanlineFormat::N24BitTcBgr,
                              result->pixels.height, result->pixels.stride))
            {
                const auto thumbnail = thumbnails.find(result->tile);
                const bool visible
                    = (thumbnail != thumbnails.end() && thumbnail->second.visible)
                      || std::any_of(viewports.begin(), viewports.end(), [&](const auto& entry) {
                             return entry.second.preview == result->tile
                                    || std::find(entry.second.visible.begin(),
                                                 entry.second.visible.end(), result->tile)
                                           != entry.second.visible.end();
                         });
                cache.put(result->tile, std::move(bitmap), cost(result->tile), !visible);
                firstContent = true;
                previewDelivered
                    |= std::any_of(viewports.begin(), viewports.end(), [&](const auto& entry) {
                           return entry.second.preview == result->tile;
                       });
            }
            else
                failed = true;
            SAL_INFO("pdffast", "native-ui-image page="
                                    << result->page << " allocation_us="
                                    << std::chrono::duration_cast<std::chrono::microseconds>(
                                           allocatedAt - imageStart)
                                           .count()
                                    << " upload_cache_us="
                                    << std::chrono::duration_cast<std::chrono::microseconds>(
                                           Clock::now() - allocatedAt)
                                           .count());
            // A sidebar-only image is consumed by its own cache polling.
            // Broadcasting ActionChanged for it rebuilds the slide sorter
            // queue and needlessly invalidates main-view primitive caches.
            if (std::any_of(viewports.begin(), viewports.end(), [&](const auto& entry) {
                    return entry.second.preview == result->tile
                           || std::find(entry.second.visible.begin(), entry.second.visible.end(),
                                        result->tile)
                                  != entry.second.visible.end();
                }))
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
    for (const auto& page : pages)
        if (!page.prepared && page.original.is()
            && (failed || changed.contains(page.original->GetGraphic().getPageNumber())))
            page.original->ActionChanged();
    // Do not resubmit missing tiles here: an over-budget view deliberately keeps
    // its preview instead of endlessly rerendering mutually evicted sharp tiles.
    bool settling = false;
    bool resume = false;
    for (auto& entry : viewports)
        if (entry.second.deferred)
        {
            if (entry.second.zoom.pending(Clock::now()))
                settling = true;
            else
            {
                entry.second.deferred = false;
                resume = true;
            }
        }
    if (resume || previewDelivered)
        schedule();
    if (editsPending || (!failed && (settling || !session->idle())))
        wake();
    else if (metadata && firstContent)
        metadataIdle.Start();
}

PdfFastDocument::PdfFastDocument(SdDrawDocument& doc)
    : m_impl(std::make_unique<Impl>(doc))
{
}
PdfFastDocument::~PdfFastDocument() = default;

void PdfFastDocument::SetMetadataReader(std::unique_ptr<vcl::PDFMetadataReader> reader)
{
    m_impl->metadata = std::move(reader);
    if (m_impl->metadata)
        for (auto& page : m_impl->pages)
            page.metadataReady = false;
}

bool PdfFastDocument::EnsureMetadata(SdrPage& page) { return m_impl->ensureMetadata(page); }

bool PdfFastDocument::EnsureAllMetadata()
{
    m_impl->metadataIdle.Stop();
    for (sal_uInt16 n = 0; n < m_impl->doc.GetSdPageCount(PageKind::Standard); ++n)
        if (!EnsureMetadata(*m_impl->doc.GetSdPage(n, PageKind::Standard)))
            return false;
    return true;
}

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

bool PdfFastDocument::PreparePaint(SdrView& view, OutputDevice& output)
{
    auto& impl = *m_impl;
    impl.devices[&output] = &view;
    const auto* pageView = view.GetSdrPageView();
    auto* entry = pageView ? impl.findPage(*pageView->GetPage()) : nullptr;
    if (impl.failed || !entry || entry->prepared)
    {
        // Materialized pages belong entirely to Draw. In particular, do not
        // redirect each imported object or invalidate its primitive cache.
        if (!impl.failed && pageView && !pageView->GetPage()->IsMasterPage())
        {
            const Impl::PaintState state{ output.GetMapMode(), output.GetOutputSizePixel(),
                                          pageView->GetPage() };
            const auto previous = impl.paintStates.find(&output);
            const auto viewport = impl.viewports.find(&output);
            if (previous != impl.paintStates.end() && previous->second.mapMode == state.mapMode
                && previous->second.pixels == state.pixels && previous->second.page == state.page
                && viewport != impl.viewports.end() && viewport->second.native)
            {
                viewport->second.seen = Clock::now();
                return false;
            }
            Impl::Viewport native;
            native.native = true;
            native.seen = Clock::now();
            if (viewport != impl.viewports.end())
                native.zoom = viewport->second.zoom;
            const Size pagePixels = output.LogicToPixel(pageView->GetPage()->GetSize());
            native.zoom.update(pageView->GetPage()->GetPageNum(), pagePixels.Width(),
                               pagePixels.Height(), native.seen);
            native.deferred = native.zoom.pending(native.seen);
            impl.collectAdjacent(*pageView->GetPage(), output, native);
            impl.paintStates.insert_or_assign(&output, state);
            impl.viewports.insert_or_assign(&output, std::move(native));
            impl.schedule();
        }
        else
        {
            impl.paintStates.erase(&output);
            if (impl.viewports.erase(&output))
                impl.schedule();
        }
        return false;
    }
    const auto* page = pageView->GetPage();
    const Impl::PaintState state{ output.GetMapMode(), output.GetOutputSizePixel(), page };
    const auto previous = impl.paintStates.find(&output);
    const auto viewport = impl.viewports.find(&output);
    if (previous != impl.paintStates.end() && previous->second.mapMode == state.mapMode
        && previous->second.pixels == state.pixels && previous->second.page == state.page
        && viewport != impl.viewports.end() && !viewport->second.native)
        return true;
    impl.paintStates.insert_or_assign(&output, state);
    // Draw caches the redirected primitive sequence. A viewport/zoom change
    // alone does not invalidate object geometry, so Paint() may never run to
    // request newly visible tiles. Invalidate before primitive traversal, not
    // inside the redirector, and only when the viewport actually changes.
    entry->original->ActionChanged();
    SAL_INFO("pdffast",
             "native-viewport pixels=" << state.pixels.Width() << 'x' << state.pixels.Height());
    return true;
}

bool PdfFastDocument::Paint(const sdr::contact::ViewObjectContact& contact, OutputDevice& output,
                            drawinglayer::primitive2d::Primitive2DDecompositionVisitor& visitor)
{
    auto& impl = *m_impl;
    auto* object = contact.GetViewContact().TryToGetSdrObject();
    if (impl.failed || !object)
        return false;
    const auto found = impl.rawPages.find(object);
    if (found == impl.rawPages.end() || found->second->prepared)
        return false;
    const auto* it = found->second;
    const int page = it->original->GetGraphic().getPageNumber();
    const auto rect = it->original->GetLogicRect();
    if (rect.IsEmpty() || it->original->GetRotateAngle() != Degree100(0)
        || it->original->GetShearAngle() != Degree100(0))
        return false;
    basegfx::B2DRange visible(rect.Left(), rect.Top(), rect.Right(), rect.Bottom());
    // Demand follows the complete window, not ViewInformation2D's paint clip.
    // A small invalidation (e.g. a delivered tile) must not cancel the rest of
    // the visible work. It also bounds requests if the paint viewport is empty.
    const auto window = output.PixelToLogic(tools::Rectangle(Point(), output.GetOutputSizePixel()));
    visible.intersect(
        basegfx::B2DRange(window.Left(), window.Top(), window.Right(), window.Bottom()));
    if (visible.isEmpty())
        return true;
    const Size pixels = output.LogicToPixel(rect.GetSize());
    const int width = std::clamp<tools::Long>(pixels.Width(), 1, 1000000);
    const int height = std::clamp<tools::Long>(pixels.Height(), 1, 1000000);
    // A zoom-independent preview survives every zoom/viewport change.
    Impl::Viewport viewport;
    viewport.preview = pdffast::pagePreview(page, rect.GetWidth(), rect.GetHeight());
    viewport.seen = Clock::now();
    if (const auto* currentPage = it->original->getSdrPageFromSdrObject())
        impl.collectAdjacent(*currentPage, output, viewport);
    const double sx = double(width) / rect.GetWidth();
    const double sy = double(height) / rect.GetHeight();
    const int left
        = std::clamp(int((visible.getMinX() - rect.Left()) * sx), 0, width - 1) / 512 * 512;
    const int top
        = std::clamp(int((visible.getMinY() - rect.Top()) * sy), 0, height - 1) / 512 * 512;
    const int right = std::clamp(int(std::ceil((visible.getMaxX() - rect.Left()) * sx)), 1, width);
    const int bottom = std::clamp(int(std::ceil((visible.getMaxY() - rect.Top()) * sy)), 1, height);
    for (int y = top; y < bottom; y += 512)
        for (int x = left; x < right; x += 512)
            viewport.visible.push_back(
                { page, width, height, x, y, std::min(512, width - x), std::min(512, height - y) });
    // One row beyond the viewport, budgeted behind every visible request.
    const int next = (bottom + 511) / 512 * 512;
    if (next < height)
        for (int x = left; x < right; x += 512)
            viewport.speculative.push_back({ page, width, height, x, next, std::min(512, width - x),
                                             std::min(512, height - next) });
    const auto previous = impl.viewports.find(&output);
    const bool changed = previous == impl.viewports.end() || previous->second.native
                         || previous->second.visible != viewport.visible
                         || previous->second.adjacent != viewport.adjacent
                         || previous->second.adjacentSharp != viewport.adjacentSharp
                         || !(previous->second.preview == viewport.preview);
    if (previous != impl.viewports.end())
        viewport.zoom = previous->second.zoom;
    viewport.zoom.update(page, width, height, viewport.seen);
    viewport.deferred = viewport.zoom.pending(viewport.seen);
    impl.viewports[&output] = viewport;
    if (changed)
        impl.schedule();

    auto draw = [&](const TileKey& key,
                    drawinglayer::primitive2d::Primitive2DDecompositionVisitor& target) {
        if (const Bitmap* bitmap = impl.cache.get(key))
        {
            const auto padded = pdffast::tileWithGutter(key);
            const double dx = double(rect.GetWidth()) / key.pageWidth;
            const double dy = double(rect.GetHeight()) / key.pageHeight;
            target.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::BitmapPrimitive2D(
                    *bitmap, basegfx::utils::createScaleTranslateB2DHomMatrix(
                                 padded.width * dx, padded.height * dy, rect.Left() + padded.x * dx,
                                 rect.Top() + padded.y * dy))));
        }
    };
    std::vector<TileKey> gaps;
    for (const auto& key : viewport.visible)
        if (!impl.cache.contains(key))
            gaps.push_back(key);
    if (!gaps.empty())
    {
        // Once all visible sharp tiles are present, avoid painting the scaled
        // full-page preview underneath them on every native Draw repaint.
        draw(viewport.preview, visitor);
        const auto candidates = impl.cache.keys([&](const TileKey& key) {
            return key.page == page && (key.pageWidth != width || key.pageHeight != height)
                   && !(key == viewport.preview)
                   && std::any_of(gaps.begin(), gaps.end(), [&](const TileKey& gap) {
                          return pdffast::fallbackIntersects(key, gap);
                      });
        });
        const auto level = pdffast::fallbackLevel(candidates, width, height);
        drawinglayer::primitive2d::Primitive2DContainer fallback;
        for (const auto& old : candidates)
            if (old.pageWidth == level.first && old.pageHeight == level.second)
                draw(old, fallback);
        if (!fallback.empty())
        {
            basegfx::B2DPolyPolygon holes;
            for (const auto& gap : gaps)
            {
                basegfx::B2DRange hole(
                    rect.Left() + double(gap.x) * rect.GetWidth() / width,
                    rect.Top() + double(gap.y) * rect.GetHeight() / height,
                    rect.Left() + double(gap.x + gap.width) * rect.GetWidth() / width,
                    rect.Top() + double(gap.y + gap.height) * rect.GetHeight() / height);
                hole.intersect(visible);
                if (!hole.isEmpty())
                    holes.append(basegfx::utils::createPolygonFromRect(hole));
            }
            visitor.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::MaskPrimitive2D(holes, std::move(fallback))));
        }
    }
    for (const auto& key : viewport.visible)
        draw(key, visitor);
    return true;
}

void PdfFastDocument::PrepareCurrentPage(SdrView& view)
{
    const auto* pageView = view.GetSdrPageView();
    if (!pageView || pageView->GetPage()->IsMasterPage())
        return;
    m_impl->preparePage(*pageView->GetPage(), &view);
}

void PdfFastDocument::SetEditMode(bool enabled) { m_impl->setEditMode(enabled); }

bool PdfFastDocument::IsEditMode() const { return m_impl->editMode; }

void PdfFastDocument::NoteScroll() { m_impl->lastScroll = Clock::now(); }

void PdfFastDocument::RequestCurrentPage(SdrView& view, const Link<bool, void>& changed)
{
    auto& impl = *m_impl;
    const bool readOnlyUI = impl.doc.GetDocSh() && impl.doc.GetDocSh()->IsReadOnlyUI();
    if (readOnlyUI)
        impl.setEditMode(false);
    else if (!impl.editMode)
        // Direct callers (including UNO/tests) use this method as the explicit
        // edit entry point. Page-change setup no longer calls it automatically.
        impl.setEditMode(true);
    impl.intents.erase(&view);
    if (changed.IsSet())
        impl.gates[&view].changed = changed;
    impl.setGate(view, EditState::Ready);
    if (!impl.editMode)
        return;
    const auto* pageView = view.GetSdrPageView();
    if (!pageView || pageView->GetPage()->IsMasterPage()
        || (impl.doc.GetDocSh() && impl.doc.GetDocSh()->IsReadOnly()))
        return;
    auto* entry = impl.findPage(*pageView->GetPage());
    if (!entry || entry->prepared)
        return;
    entry->failed = false; // An explicit revisit/edit action is a retry.
    impl.setGate(view, EditState::Preparing);
    impl.intents[&view] = { entry->original, Clock::now() + std::chrono::milliseconds(120) };
    // Remove stale high-resolution PDF work immediately. The conversion
    // helper and the native Draw renderer must not compete with old tiles.
    impl.schedule();
    impl.wake();
}

PdfFastDocument::EditState PdfFastDocument::GetEditState(const SdrView& view) const
{
    const auto it = m_impl->gates.find(const_cast<SdrView*>(&view));
    return it == m_impl->gates.end() ? EditState::Ready : it->second.state;
}

void PdfFastDocument::ForgetView(const SdrView& view)
{
    auto& impl = *m_impl;
    impl.intents.erase(const_cast<SdrView*>(&view));
    // Do not invoke callbacks while sd::View or its owning shell is destructing.
    impl.gates.erase(const_cast<SdrView*>(&view));
    for (auto it = impl.devices.begin(); it != impl.devices.end();)
        if (it->second == &view)
        {
            impl.viewports.erase(it->first);
            impl.paintStates.erase(it->first);
            it = impl.devices.erase(it);
        }
        else
            ++it;
    impl.schedule();
}

bool PdfFastDocument::Impl::processEdits()
{
    // View destruction unregisters synchronously. Never retain page pointers in
    // jobs: the original object's current model membership is authoritative.
    for (auto it = intents.begin(); it != intents.end();)
    {
        const auto* current = it->first->GetSdrPageView();
        const auto* page = it->second.original->getSdrPageFromSdrObject();
        if (!current || !page || current->GetPage() != page
            || &page->getSdrModelFromSdrPage() != &doc
            || (doc.GetDocSh() && doc.GetDocSh()->IsReadOnly()))
        {
            setGate(*it->first, PdfFastDocument::EditState::Ready);
            it = intents.erase(it);
        }
        else
            ++it;
    }
    if (running.is())
    {
        const bool wanted = std::any_of(intents.begin(), intents.end(), [&](const auto& intent) {
            return intent.second.original == running;
        });
        if (!wanted)
            importer->cancel();
        if (!pendingEditResult)
            pendingEditResult = importer->takeResult();
        if (pendingEditResult && wanted && pendingEditResult->success
            && (Clock::now() - lastScroll < std::chrono::milliseconds(250)
                || Application::GetLastInputInterval() < 250
                || Application::AnyInput(VclInputFlags::MOUSE | VclInputFlags::KEYBOARD)))
            return true;
        if (pendingEditResult)
        {
            auto result = std::move(pendingEditResult);
            pendingEditResult.reset();
            auto original = running;
            running.clear();
            comphelper::ScopeGuard release([&] { importer->release(); });
            auto* page = original->getSdrPageFromSdrObject();
            const auto link = original->GetGraphic().GetSharedGfxLink();
            bool ready = false;
            if (wanted && page && &page->getSdrModelFromSdrPage() == &doc && link
                && link->getDataContainer().getDataShared() == source && result->page == runningPage
                && result->page == original->GetGraphic().getPageNumber())
            {
                if (!result->success)
                    SAL_WARN("pdffast", "Native page preparation failed: " << result->error);
                else
                    ready = preparePage(*page, nullptr, &*result);
                if (auto* entry = findPage(*page))
                    entry->failed = !ready;
                original->ActionChanged();
            }
            // Complete/fail only this intent; edits made elsewhere while the
            // worker ran keep their undo history and current modified state.
            for (auto it = intents.begin(); it != intents.end();)
                if (it->second.original == original)
                {
                    setGate(*it->first, ready ? PdfFastDocument::EditState::Ready
                                              : PdfFastDocument::EditState::Failed);
                    it = intents.erase(it);
                }
                else
                    ++it;
            // A failed conversion may continue displaying the read-only PDF;
            // rebuild demand after removing the edit intent.
            schedule();
        }
    }
    if (!running.is())
        for (const auto& entry : intents)
            if (Clock::now() >= entry.second.due)
            {
                const int page = entry.second.original->GetGraphic().getPageNumber();
                if (importer->request(page))
                {
                    running = entry.second.original;
                    runningPage = page;
                }
                break;
            }
    return running.is() || !intents.empty();
}

IMPL_LINK_NOARG(PdfFastDocument::Impl, EditCompleted, void*, void)
{
    {
        std::scoped_lock lock(editEventMutex);
        editEvent = nullptr;
    }
    // Completion must wake the UI independently of preview/zoom timer delivery.
    if (processEdits())
        wake();
}

bool PdfFastDocument::PrepareForSave()
{
    comphelper::ScopeGuard refresh([&] {
        for (const auto& gate : m_impl->gates)
        {
            auto* pageView = gate.first->GetSdrPageView();
            auto* entry = pageView ? m_impl->findPage(*pageView->GetPage()) : nullptr;
            m_impl->setGate(*gate.first, entry && !entry->prepared && m_impl->doc.GetDocSh()
                                                 && !m_impl->doc.GetDocSh()->IsReadOnly()
                                             ? EditState::Failed
                                             : EditState::Ready);
        }
    });
    m_impl->intents.clear();
    if (!EnsureAllMetadata())
        return false;
    // A regular ODG must remain fully editable without this PDF-only adapter.
    // Convert before opening the output storage; never serialize an incomplete
    // deferred-edit contract that cannot be reconstructed on reopen.
    for (sal_uInt16 n = 0; n < m_impl->doc.GetSdPageCount(PageKind::Standard); ++n)
        if (!m_impl->preparePage(*m_impl->doc.GetSdPage(n, PageKind::Standard), nullptr))
            return false;
    return true;
}

PdfFastDocument::PreviewResult
PdfFastDocument::GetPagePreview(const SdrPage& page, const Size& size, Bitmap& bitmap, bool visible)
{
    auto& impl = *m_impl;
    if (impl.failed || size.Width() <= 0 || size.Height() <= 0 || page.GetObjCount() != 1)
        return PreviewResult::NotHandled;
    auto* entry = impl.findPage(const_cast<SdrPage&>(page));
    if (!entry || entry->prepared)
        return PreviewResult::NotHandled;
    // Only substitute an untouched full-page PDF. Cropped/transformed graphics
    // or additional Draw objects still need the normal composite preview.
    const auto& rect = entry->original->GetLogicRect();
    if (rect.TopLeft() != Point() || rect.GetSize() != page.GetSize()
        || entry->original->GetRotateAngle() != Degree100(0)
        || entry->original->GetShearAngle() != Degree100(0))
        return PreviewResult::NotHandled;
    if (page.GetSize().Width() <= 0 || page.GetSize().Height() <= 0)
        return PreviewResult::NotHandled;
    // Preserve aspect ratio and keep a single result below the worker's
    // bounded output budget, even for oversized preview requests.
    const double scale
        = std::min(double(std::min<tools::Long>(size.Width(), 1024)) / page.GetSize().Width(),
                   double(std::min<tools::Long>(size.Height(), 1024)) / page.GetSize().Height());
    const int width = std::max(1, int(page.GetSize().Width() * scale));
    const int height = std::max(1, int(page.GetSize().Height() * scale));
    const auto preview = pdffast::pagePreview(entry->original->GetGraphic().getPageNumber(),
                                              page.GetSize().Width(), page.GetSize().Height());
    // Share the main window's zoom-independent preview for ordinary sidebar
    // sizes. A page is parsed/rasterized once for both first useful images.
    const TileKey key = width <= preview.width && height <= preview.height
                            ? preview
                            : TileKey{ preview.page, width, height, 0, 0, width, height };
    impl.thumbnails[key] = { Clock::now(), visible };
    if (const auto* cached = impl.cache.get(key))
    {
        bitmap = *cached;
        if (bitmap.GetSizePixel() != Size(width, height))
            bitmap.Scale(Size(width, height));
        return PreviewResult::Ready;
    }
    impl.schedule();
    return PreviewResult::Pending;
}

bool PdfFastDocument::Impl::preparePage(SdrPage& target, SdrView* view,
                                        const pdffast::NativeImport::Result* completed)
{
    auto& impl = *this;
    if (impl.preparing)
        return false;
    auto* it = impl.findPage(target);
    if (!it || it->prepared)
        return true;
    if (!impl.ensureMetadata(target))
        return false;
    if (!completed)
    {
        // Explicit save/tests retain a synchronous barrier. Normal navigation
        // uses RequestCurrentPage and never waits for helper work on the UI.
        if (impl.running.is())
        {
            impl.importer->cancel();
            if (impl.pendingEditResult)
                impl.pendingEditResult.reset();
            else
                impl.importer->wait();
            impl.importer->release();
            impl.running.clear();
        }
        if (!impl.importer->request(it->original->GetGraphic().getPageNumber()))
            return false;
        auto result = impl.importer->wait();
        comphelper::ScopeGuard release([&] { impl.importer->release(); });
        if (!result.success)
        {
            it->failed = true;
            SAL_WARN("pdffast", "Native single-page transport failed: " << result.error);
            return false;
        }
        return preparePage(target, view, &result);
    }
    auto* page = &target;
    impl.preparing = true;
    comphelper::ScopeGuard resetPreparing([&] { impl.preparing = false; });
    const auto started = Clock::now();
    const bool changed = impl.doc.IsChanged();
    const bool modified = impl.doc.GetDocSh() && impl.doc.GetDocSh()->IsModified();
    const bool undo = impl.doc.IsUndoEnabled();
    const auto originalRect = it->original->GetLogicRect();
    SAL_INFO("pdffast",
             "native-edit-begin page="
                 << page->GetPageNum() << " pdf_page=" << it->original->GetGraphic().getPageNumber()
                 << " x=" << originalRect.Left() << " y=" << originalRect.Top()
                 << " width=" << originalRect.GetWidth() << " height=" << originalRect.GetHeight());
    // Other windows and pages retain their own raster demand during import.
    for (auto viewport = impl.viewports.begin(); viewport != impl.viewports.end();)
        if ((!viewport->second.native
             && viewport->second.preview.page == it->original->GetGraphic().getPageNumber())
            || std::any_of(viewport->second.adjacent.begin(), viewport->second.adjacent.end(),
                           [&](const TileKey& key) {
                               return key.page == it->original->GetGraphic().getPageNumber();
                           }))
            viewport = impl.viewports.erase(viewport);
        else
            ++viewport;
    impl.schedule();
    comphelper::ScopeGuard restore([&] {
        if (view)
            view->UnmarkAllObj();
        impl.doc.EnableUndo(undo);
        impl.doc.SetChanged(changed);
        if (auto* shell = impl.doc.GetDocSh())
            shell->SetModified(modified);
    });
    impl.doc.EnableUndo(false);
    if (view)
        view->UnmarkAllObj();
    try
    {
        auto staged = impl.importEditablePage(it->original->GetGraphic(), *completed);
        auto* stagedPage = dynamic_cast<SdPage*>(staged.get());
        auto* targetPage = dynamic_cast<SdPage*>(page);
        if (!stagedPage || !targetPage)
            throw std::runtime_error("Missing native Draw page");
        const auto originalPosition = it->original->GetOrdNum();
        std::vector<rtl::Reference<SdrObject>> objects;
        objects.reserve(staged->GetObjCount());
        while (staged->GetObjCount())
            objects.push_back(staged->NbcRemoveObject(staged->GetObjCount() - 1));
        std::reverse(objects.begin(), objects.end());
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
        // Page geometry, background and annotations already belong to the live
        // Draw document. Materialization replaces only the source graphic;
        // importing must preserve page-format changes and edits made elsewhere.
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
    SAL_INFO("pdffast",
             "native-edit-prepare page="
                 << page->GetPageNum() << " success=" << it->prepared
                 << " objects=" << page->GetObjCount() << " total_us="
                 << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started)
                        .count());
    return it->prepared;
}
}
