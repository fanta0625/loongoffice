/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <PdfFastDocument.hxx>
#include <DrawDocShell.hxx>
#include <drawdoc.hxx>
#include <sdpage.hxx>
#include <sdpdffilter.hxx>
#include <PreviewRenderer.hxx>
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
#include <svl/lstner.hxx>
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
#include <vcl/settings.hxx>
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

struct PdfFastDocument::Impl : SfxListener
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
        TileKey preview;
        Clock::time_point seen;
        pdffast::ZoomSettle zoom;
        bool deferred = false;
    };
    struct PaintState
    {
        MapMode mapMode;
        Size pixels;
        const SdrPage* page;
    };
    SdDrawDocument& doc;
    std::deque<Page> pages;
    std::unique_ptr<pdffast::Session> session;
    pdffast::Cache<TileKey, Bitmap> cache{ 128 * 1024 * 1024 };
    // Device addresses are identity only; never dereferenced or retained as windows.
    std::map<const OutputDevice*, Viewport> viewports;
    std::map<const OutputDevice*, PaintState> paintStates;
    std::map<const OutputDevice*, const SdrView*> devices;
    std::map<TileKey, Clock::time_point> thumbnails;
    struct NativeViewport
    {
        rtl::Reference<SdPage> page;
        sal_uInt64 revision = 0;
        sal_uInt64 masterRevision = 0;
        sal_uInt64 epoch = 0;
        SdrLayerIDSet layers;
        SdrLayerIDSet printable;
        Color documentColor;
        StyleSettings styleSettings;
        int id = 0;
        TileKey preview;
        std::vector<TileKey> visible;
        pdffast::ZoomSettle zoom;
        bool deferred = false;
        bool emitted = false;
        bool failed = false;
    };
    std::map<const OutputDevice*, NativeViewport> nativeViews;
    pdffast::Cache<TileKey, Bitmap> nativeCache{ 64 * 1024 * 1024 };
    PreviewRenderer nativeRenderer{ false };
    Idle nativeIdle{ "Draw PDF native read-only snapshots" };
    sal_uInt64 nativeEpoch = 0;
    int nativeId = 0;
    bool nativePreviewOnly = false;
    void Notify(SfxBroadcaster&, const SfxHint&) override
    {
        // Object insertion/removal, undo, layers, styles and master-page changes
        // can affect several pages. Local NBC changes are covered by revisions.
        ++nativeEpoch;
    }
    bool nativeCurrent(const NativeViewport& viewport) const
    {
        return viewport.epoch == nativeEpoch
            && viewport.revision == viewport.page->GetVisualRevision()
            && viewport.masterRevision == (viewport.page->TRG_HasMasterPage()
                ? viewport.page->TRG_GetMasterPage().GetVisualRevision() : 0);
    }
    static void invalidateNative(SdrPage& page)
    {
        // Invalidate redirected primitive caches without changing model versions.
        for (std::size_t i = 0; i < page.GetObjCount(); ++i)
            page.GetObj(i)->GetViewContact().ActionChanged();
    }
    void scheduleNative();
    bool prepareNative(SdrView&, OutputDevice&);
    bool paintNative(SdrObject&, OutputDevice&,
                     drawinglayer::primitive2d::Primitive2DDecompositionVisitor&);
    DECL_LINK(RenderNative, Timer*, void);
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
    Timer poll{ "Draw PDF tile delivery" };
    Idle metadataIdle{ "Draw PDF deferred metadata" };
    std::mutex editEventMutex;
    ImplSVEvent* editEvent = nullptr;
    DECL_LINK(EditCompleted, void*, void);
    std::unique_ptr<vcl::PDFMetadataReader> metadata;
    bool firstContent = false;
    bool failed = false;
    bool preparing = false;
    bool editMode = false;

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
        for (auto& [view, gate] : gates)
            if (gate.state != PdfFastDocument::EditState::Ready)
                setGate(*view, PdfFastDocument::EditState::Ready);
        if (running.is())
            importer->cancel();
        poll.Start();
    }

    explicit Impl(SdDrawDocument& document) : doc(document)
    {
        StartListening(doc);
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
        nativeIdle.SetPriority(TaskPriority::LOWEST);
        nativeIdle.SetInvokeHandler(LINK(this, Impl, RenderNative));
        poll.Start();
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
            auto found = std::find_if(pages.begin(), pages.end(),
                [&](const Page& entry) { return entry.original.get() == object; });
            if (found != pages.end())
                return &*found;
            pages.push_back({ object, false, false });
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
                    if (std::find(annotations.begin(), annotations.end(), annotation) == annotations.end())
                        page.removeAnnotationNoNotify(annotation);
                page.setLinkAnnotations(links);
            });
            SdPdfFilter::ApplyPageMetadata(page, *result);
            applied = true;
            entry->metadataReady = true;
            entry->metadataFailed = false;
            target.ActionChanged();
            SAL_INFO("pdffast", "native-metadata page=" << result->GetGraphic().getPageNumber()
                     << " annotations=" << result->GetAnnotations().size()
                     << " links=" << result->GetLinksInfo().size() << " total_us="
                     << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
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
        nativeIdle.Stop();
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
        auto* nativeShell = new DrawDocShell(SfxObjectCreateMode::EMBEDDED, true, DocumentType::Draw);
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
        uno::Reference<lang::XInitialization>(filter, uno::UNO_QUERY_THROW)->initialize(
            { uno::Any(definition) });
        uno::Reference<document::XImporter>(filter, uno::UNO_QUERY_THROW)->setTargetDocument(
            nativeShell->GetModel());

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
        for (auto it = thumbnails.begin(); it != thumbnails.end();)
            if (now - it->second > std::chrono::seconds(2))
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
            previews[viewport.preview] = cost(viewport.preview);
            visible[viewport.preview] = cost(viewport.preview);
            add(viewport.preview, false);
        }
        for (const auto& entry : viewports)
        {
            // Page navigation is preview-first. Do not enqueue all visible
            // high-resolution tiles until the new page's bounded preview has
            // arrived; on a complex page those tiles can otherwise delay the
            // first useful frame by several seconds on slower CPUs.
            if (entry.second.zoom.pending(now) || !cache.contains(entry.second.preview))
                continue;
            for (const auto& key : entry.second.visible)
            {
                visible[key] = cost(key);
                add(key, false);
            }
        }
        // Main-window demand always precedes sidebar work.
        for (const auto& entry : thumbnails)
        {
            previews[entry.first] = cost(entry.first);
            visible[entry.first] = cost(entry.first);
            add(entry.first, false);
        }
        cache.protect(std::move(visible), std::move(previews));
        std::size_t spare = cache.spareForSpeculation();
        for (const auto& entry : viewports)
        {
            if (entry.second.zoom.pending(now))
                continue;
            for (const auto& key : entry.second.speculative)
                if (cost(key) <= spare && !cache.contains(key))
                {
                    spare -= cost(key);
                    add(key, true);
                }
        }
        session->request(std::move(requests));
        if (!failed)
            poll.Start();
    }

    DECL_LINK(Deliver, Timer*, void);
    DECL_LINK(ReadMetadata, Timer*, void);
};

void PdfFastDocument::Impl::scheduleNative()
{
    std::map<TileKey, std::size_t> visible, previews;
    bool pending = false;
    bool settling = false;
    for (auto& [device, viewport] : nativeViews)
    {
        (void)device;
        if (viewport.failed || !nativeCurrent(viewport))
            continue;
        previews[viewport.preview] = cost(viewport.preview);
        pending |= !nativeCache.contains(viewport.preview);
        viewport.deferred = viewport.zoom.pending(Clock::now());
        settling |= viewport.deferred;
        if (!viewport.deferred)
            for (const auto& key : viewport.visible)
            {
                visible[key] = cost(key);
                pending |= !nativeCache.contains(key);
            }
    }
    std::size_t workingSet = 0;
    for (const auto& entry : visible)
        workingSet += entry.second;
    for (const auto& entry : previews)
        workingSet += entry.second;
    nativePreviewOnly = workingSet > 64 * 1024 * 1024;
    if (nativePreviewOnly)
    {
        visible.clear();
        pending = std::any_of(previews.begin(), previews.end(),
            [&](const auto& entry) { return !nativeCache.contains(entry.first); });
    }
    nativeCache.protect(std::move(visible), std::move(previews));
    if (pending)
        nativeIdle.Start();
    if (settling)
        poll.Start();
}

bool PdfFastDocument::Impl::prepareNative(SdrView& view, OutputDevice& output)
{
    auto* pageView = view.GetSdrPageView();
    auto* page = pageView ? dynamic_cast<SdPage*>(pageView->GetPage()) : nullptr;
    // Editing, nonstandard rendering modes and objects outside the page retain
    // Draw's live object rendering, including handles and out-of-page content.
    auto* raw = page ? findPage(*page) : nullptr;
    const bool eligible = doc.GetDocSh() && doc.GetDocSh()->IsReadOnly() && page
        && !page->IsMasterPage() && page->GetPageKind() == PageKind::Standard
        && page->GetObjCount() && !page->GetSize().IsEmpty()
        && (!raw || raw->prepared) && output.GetDrawMode() == DrawModeFlags::Default
        && !Application::GetSettings().GetStyleSettings().GetHighContrastMode()
        && tools::Rectangle(Point(), page->GetSize()).Contains(page->GetAllObjBoundRect());
    auto previous = nativeViews.find(&output);
    if (!eligible)
    {
        if (previous != nativeViews.end())
        {
            invalidateNative(*previous->second.page);
            nativeViews.erase(previous);
            scheduleNative();
        }
        return false;
    }
    const bool replace = previous == nativeViews.end() || previous->second.page.get() != page
        || !nativeCurrent(previous->second)
        || previous->second.layers != pageView->GetVisibleLayers()
        || previous->second.printable != pageView->GetPrintableLayers()
        || previous->second.documentColor != pageView->GetApplicationDocumentColor()
        || !(previous->second.styleSettings == Application::GetSettings().GetStyleSettings());
    if (replace)
    {
        if (previous != nativeViews.end())
            nativeViews.erase(previous);
        NativeViewport viewport;
        viewport.page = page;
        viewport.revision = page->GetVisualRevision();
        viewport.masterRevision = page->TRG_HasMasterPage()
            ? page->TRG_GetMasterPage().GetVisualRevision() : 0;
        viewport.epoch = nativeEpoch;
        viewport.layers = pageView->GetVisibleLayers();
        viewport.printable = pageView->GetPrintableLayers();
        viewport.documentColor = pageView->GetApplicationDocumentColor();
        viewport.styleSettings = Application::GetSettings().GetStyleSettings();
        if (nativeId == std::numeric_limits<int>::max())
        {
            nativeViews.clear();
            nativeCache.clear();
            nativeId = 0;
        }
        viewport.id = ++nativeId;
        previous = nativeViews.emplace(&output, std::move(viewport)).first;
    }
    auto& viewport = previous->second;
    viewport.emitted = false;
    const auto size = page->GetSize();
    const auto pixels = output.LogicToPixel(size);
    const int width = std::clamp<tools::Long>(pixels.Width(), 1, 1000000);
    const int height = std::clamp<tools::Long>(pixels.Height(), 1, 1000000);
    const auto visible = output.PixelToLogic(tools::Rectangle(Point(), output.GetOutputSizePixel()))
        .GetIntersection(tools::Rectangle(Point(), size));
    const double scale = std::min(256.0 / size.Width(), 512.0 / size.Height());
    const int pw = std::max(1, int(size.Width() * scale));
    const int ph = std::max(1, int(size.Height() * scale));
    viewport.preview = { viewport.id, pw, ph, 0, 0, pw, ph };
    const auto oldVisible = viewport.visible;
    viewport.visible.clear();
    if (!visible.IsEmpty())
    {
        const double sx = double(width) / size.Width();
        const double sy = double(height) / size.Height();
        const int left = std::clamp(int(visible.Left() * sx), 0, width - 1) / 512 * 512;
        const int top = std::clamp(int(visible.Top() * sy), 0, height - 1) / 512 * 512;
        const int right = std::clamp(int(std::ceil(visible.Right() * sx)), 1, width);
        const int bottom = std::clamp(int(std::ceil(visible.Bottom() * sy)), 1, height);
        for (int y = top; y < bottom; y += 512)
            for (int x = left; x < right; x += 512)
                viewport.visible.push_back({ viewport.id, width, height, x, y,
                    std::min(512, width - x), std::min(512, height - y) });
    }
    viewport.zoom.update(viewport.id, width, height, Clock::now());
    if (replace || oldVisible != viewport.visible)
        invalidateNative(*page);
    scheduleNative();
    return true;
}

bool PdfFastDocument::Impl::paintNative(SdrObject& object, OutputDevice& output,
    drawinglayer::primitive2d::Primitive2DDecompositionVisitor& visitor)
{
    const auto found = nativeViews.find(&output);
    if (found == nativeViews.end() || found->second.failed || !nativeCurrent(found->second)
        || object.getSdrPageFromSdrObject() != found->second.page.get()
        || !nativeCache.contains(found->second.preview))
        return false;
    auto& viewport = found->second;
    // The redirector visits the visible native objects in order. One composite
    // replaces them for this window repaint; editing always uses live objects.
    if (viewport.emitted)
        return true;
    viewport.emitted = true;
    const auto size = viewport.page->GetSize();
    auto draw = [&](const TileKey& key,
                    drawinglayer::primitive2d::Primitive2DDecompositionVisitor& target) {
        if (const auto* bitmap = nativeCache.get(key))
        {
            const double dx = double(size.Width()) / key.pageWidth;
            const double dy = double(size.Height()) / key.pageHeight;
            target.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::BitmapPrimitive2D(*bitmap,
                    basegfx::utils::createScaleTranslateB2DHomMatrix(
                        key.width * dx, key.height * dy, key.x * dx, key.y * dy))));
        }
    };
    std::vector<TileKey> gaps;
    for (const auto& key : viewport.visible)
        if (!nativeCache.contains(key))
            gaps.push_back(key);
    if (!gaps.empty())
    {
        draw(viewport.preview, visitor);
        const int width = gaps.front().pageWidth;
        const int height = gaps.front().pageHeight;
        const auto candidates = nativeCache.keys([&](const TileKey& key) {
            return key.page == viewport.id && !(key == viewport.preview)
                && (key.pageWidth != width || key.pageHeight != height)
                && std::any_of(gaps.begin(), gaps.end(), [&](const TileKey& gap) {
                    return pdffast::fallbackIntersects(key, gap);
                });
        });
        const auto level = pdffast::fallbackLevel(candidates, width, height);
        drawinglayer::primitive2d::Primitive2DContainer fallback;
        for (const auto& key : candidates)
            if (key.pageWidth == level.first && key.pageHeight == level.second)
                draw(key, fallback);
        if (!fallback.empty())
        {
            basegfx::B2DPolyPolygon holes;
            for (const auto& gap : gaps)
                holes.append(basegfx::utils::createPolygonFromRect(basegfx::B2DRange(
                    double(gap.x) * size.Width() / width,
                    double(gap.y) * size.Height() / height,
                    double(gap.x + gap.width) * size.Width() / width,
                    double(gap.y + gap.height) * size.Height() / height)));
            visitor.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::MaskPrimitive2D(holes, std::move(fallback))));
        }
    }
    for (const auto& key : viewport.visible)
        draw(key, visitor);
    return true;
}

IMPL_LINK_NOARG(PdfFastDocument::Impl, RenderNative, Timer*, void)
{
    if (preparing || running.is() || !intents.empty())
        return; // Deliver restarts snapshot work after the edit commits.
    for (auto& [device, viewport] : nativeViews)
    {
        const auto owner = devices.find(device);
        if (viewport.failed || !nativeCurrent(viewport) || owner == devices.end()
            || !doc.GetDocSh() || !doc.GetDocSh()->IsReadOnly())
            continue;
        const auto* pageView = owner->second->GetSdrPageView();
        if (!pageView || pageView->GetPage() != viewport.page.get()
            || pageView->GetVisibleLayers() != viewport.layers
            || pageView->GetPrintableLayers() != viewport.printable
            || pageView->GetApplicationDocumentColor() != viewport.documentColor
            || !(viewport.styleSettings == Application::GetSettings().GetStyleSettings()))
            continue;
        std::optional<TileKey> job;
        if (!nativeCache.contains(viewport.preview))
            job = viewport.preview;
        else if (!nativePreviewOnly && !viewport.zoom.pending(Clock::now()))
            for (const auto& key : viewport.visible)
                if (!nativeCache.contains(key))
                {
                    job = key;
                    break;
                }
        if (!job)
            continue;
        const auto started = Clock::now();
        auto bitmap = nativeRenderer.RenderRegion(*viewport.page,
            Size(job->pageWidth, job->pageHeight),
            tools::Rectangle(Point(job->x, job->y), Size(job->width, job->height)), *pageView);
        if (!nativeCurrent(viewport))
        {
            invalidateNative(*viewport.page);
            return;
        }
        // Rejected admission (an oversized visible working set) degrades to
        // native rendering instead of rerendering mutually evicted tiles forever.
        if (bitmap.IsEmpty() || !nativeCache.put(*job, std::move(bitmap), cost(*job)))
            viewport.failed = true;
        SAL_INFO("pdffast", "native-read-snapshot page=" << viewport.page->GetPageNum()
            << " width=" << job->width << " height=" << job->height << " total_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
        invalidateNative(*viewport.page);
        scheduleNative();
        return; // At most one bounded native surface per idle callback.
    }
}

IMPL_LINK_NOARG(PdfFastDocument::Impl, ReadMetadata, Timer*, void)
{
    if (preparing || running.is() || !intents.empty() || !session->idle())
        return; // Delivery restarts the idle after interactive work.
    // Visible pages get their links and annotations before background pages.
    for (const auto& [device, view] : devices)
        if (auto* pageView = view->GetSdrPageView())
            if (auto* entry = findPage(*pageView->GetPage());
                entry && !entry->metadataReady && !entry->metadataFailed)
            {
                ensureMetadata(*pageView->GetPage());
                metadataIdle.Start();
                return;
            }
    for (auto& entry : pages)
        if (!entry.metadataReady && !entry.metadataFailed && entry.original.is())
            if (auto* page = entry.original->getSdrPageFromSdrObject())
            {
                // One page per idle: input and repaint tasks take precedence.
                ensureMetadata(*page);
                metadataIdle.Start();
                return;
            }
}

IMPL_LINK_NOARG(PdfFastDocument::Impl, Deliver, Timer*, void)
{
    const bool editsPending = processEdits();
    const auto start = Clock::now();
    std::set<int> changed;
    bool previewDelivered = false;
    for (std::size_t items = 0;
         pdffast::TransferBudget::allows(items, Clock::now() - start); ++items)
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
            if (allocated && ReadRawDIB(bitmap, result->pixels.data.data(), ScanlineFormat::N24BitTcBgr,
                           result->pixels.height, result->pixels.stride))
            {
                const bool visible = thumbnails.contains(result->tile)
                    || std::any_of(viewports.begin(), viewports.end(),
                    [&](const auto& entry) {
                        return entry.second.preview == result->tile
                            || std::find(entry.second.visible.begin(), entry.second.visible.end(),
                                         result->tile) != entry.second.visible.end();
                    });
                cache.put(result->tile, std::move(bitmap), cost(result->tile), !visible);
                firstContent = true;
                previewDelivered |= std::any_of(viewports.begin(), viewports.end(),
                    [&](const auto& entry) { return entry.second.preview == result->tile; });
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
    if (!nativeViews.empty())
        scheduleNative();
    if (editsPending || (!failed && (settling || !session->idle())))
        poll.Start();
    else if (metadata && firstContent)
        metadataIdle.Start();
}

PdfFastDocument::PdfFastDocument(SdDrawDocument& doc) : m_impl(std::make_unique<Impl>(doc)) {}
PdfFastDocument::~PdfFastDocument() = default;

void PdfFastDocument::SetMetadataReader(std::unique_ptr<vcl::PDFMetadataReader> reader)
{
    m_impl->metadata = std::move(reader);
    if (m_impl->metadata)
        for (auto& page : m_impl->pages)
            page.metadataReady = false;
}

bool PdfFastDocument::EnsureMetadata(SdrPage& page)
{
    return m_impl->ensureMetadata(page);
}

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

void PdfFastDocument::PreparePaint(SdrView& view, OutputDevice& output)
{
    auto& impl = *m_impl;
    impl.devices[&output] = &view;
    if (impl.prepareNative(view, output))
        return;
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
    auto* object = contact.GetViewContact().TryToGetSdrObject();
    if (object && impl.paintNative(*object, output, visitor))
        return true;
    if (impl.failed)
        return false;
    if (object && object->getSdrPageFromSdrObject())
        impl.findPage(*object->getSdrPageFromSdrObject());
    auto it = std::find_if(impl.pages.begin(), impl.pages.end(),
        [&](const Impl::Page& page) { return page.original.get() == object && !page.prepared; });
    if (it == impl.pages.end())
        return false;
    const int page = it->original->GetGraphic().getPageNumber();
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
            const double dx = double(rect.GetWidth()) / key.pageWidth;
            const double dy = double(rect.GetHeight()) / key.pageHeight;
            target.visit(drawinglayer::primitive2d::Primitive2DReference(
                new drawinglayer::primitive2d::BitmapPrimitive2D(*bitmap,
                    basegfx::utils::createScaleTranslateB2DHomMatrix(
                        key.width * dx, key.height * dy,
                        rect.Left() + key.x * dx, rect.Top() + key.y * dy))));
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
                basegfx::B2DRange hole(rect.Left() + double(gap.x) * rect.GetWidth() / width,
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

void PdfFastDocument::SetEditMode(bool enabled)
{
    m_impl->setEditMode(enabled);
}

bool PdfFastDocument::IsEditMode() const
{
    return m_impl->editMode;
}

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
    impl.poll.Start();
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
            impl.nativeViews.erase(it->first);
            it = impl.devices.erase(it);
        }
        else
            ++it;
    impl.schedule();
    impl.scheduleNative();
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
        if (auto result = importer->takeResult())
        {
            auto original = running;
            running.clear();
            comphelper::ScopeGuard release([&] { importer->release(); });
            auto* page = original->getSdrPageFromSdrObject();
            const auto link = original->GetGraphic().GetSharedGfxLink();
            bool ready = false;
            if (wanted && page && &page->getSdrModelFromSdrPage() == &doc && link
                && link->getDataContainer().getDataShared() == source
                && result->page == runningPage
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
        poll.Start();
}

bool PdfFastDocument::PrepareForSave()
{
    comphelper::ScopeGuard refresh([&] {
        for (const auto& gate : m_impl->gates)
        {
            auto* pageView = gate.first->GetSdrPageView();
            auto* entry = pageView ? m_impl->findPage(*pageView->GetPage()) : nullptr;
            m_impl->setGate(*gate.first, entry && !entry->prepared
                && m_impl->doc.GetDocSh() && !m_impl->doc.GetDocSh()->IsReadOnly()
                ? EditState::Failed : EditState::Ready);
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

PdfFastDocument::PreviewResult PdfFastDocument::GetPagePreview(
    const SdrPage& page, const Size& size, Bitmap& bitmap)
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
    const double scale = std::min(double(std::min<tools::Long>(size.Width(), 1024))
                                      / page.GetSize().Width(),
                                  double(std::min<tools::Long>(size.Height(), 1024))
                                      / page.GetSize().Height());
    const int width = std::max(1, int(page.GetSize().Width() * scale));
    const int height = std::max(1, int(page.GetSize().Height() * scale));
    const TileKey key{ entry->original->GetGraphic().getPageNumber(), width, height,
                       0, 0, width, height };
    impl.thumbnails[key] = Clock::now();
    if (const auto* cached = impl.cache.get(key))
    {
        bitmap = *cached;
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
    SAL_INFO("pdffast", "native-edit-begin page=" << page->GetPageNum()
        << " pdf_page=" << it->original->GetGraphic().getPageNumber()
        << " x=" << originalRect.Left() << " y=" << originalRect.Top()
        << " width=" << originalRect.GetWidth() << " height=" << originalRect.GetHeight());
    // Other windows and pages retain their own raster demand during import.
    for (auto viewport = impl.viewports.begin(); viewport != impl.viewports.end();)
        if (viewport->second.preview.page == it->original->GetGraphic().getPageNumber())
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
    SAL_INFO("pdffast", "native-edit-prepare page=" << page->GetPageNum()
        << " success=" << it->prepared << " objects=" << page->GetObjCount() << " total_us="
        << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
    return it->prepared;
}
}
