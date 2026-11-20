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
#include <vcl/pdfextoutdevdata.hxx>
#include <o3tl/unit_conversion.hxx>
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
#include <sfx2/progress.hxx>
#include <sdresid.hxx>
#include <strings.hrc>
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
#include <tools/stream.hxx>
#include <unotools/tempfile.hxx>
#include <sal/log.hxx>
#include <comphelper/scopeguard.hxx>
#include <algorithm>
#include <array>
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
// Every page activation (mode switch, click, UNO, navigation) waits only
// briefly for the input queue to drain before committing on the UI thread.
// The replay itself is interruptible: input that arrives while it runs
// abandons it within a few milliseconds (LoongOfficePdfInterruptible), so
// there is no need for a long quiet window before conversion starts.
constexpr auto COMMIT_QUIET = std::chrono::milliseconds(120);
constexpr auto REQUEST_DEBOUNCE = std::chrono::milliseconds(120);
// Incremental commit: object insertion is spread over bounded UI-thread
// slices so page flipping and scrolling never wait for a whole page. The
// budget is one part of a frame; input between slices keeps priority.
constexpr auto COMMIT_SLICE = std::chrono::milliseconds(8);
// Staging was abandoned because user input arrived. The page is not broken;
// its pending edit intent retries after the debounce.
struct StagingInterrupted
{
};
bool inputPending()
{
    // Non-dispatching peek at the local event queue only, mouse and keyboard.
    // "Any input" would also include the Qt/KF6 backend's event-loop state
    // (OTHER / !sleeping), which the replay pump flips itself; that would
    // report input that does not exist. Undelivered input is covered by
    // stagingInputPending()'s last-input timestamp.
    return Application::AnyInput(VclInputFlags::MOUSE | VclInputFlags::KEYBOARD);
}
// The private replay (sdext) pumps the event loop while this thread is busy,
// so input it dispatched is already consumed from the toolkit queue. Backends
// which cannot peek at that queue (Qt/KF6) expose it only through the VCL
// last-input timestamp. 50ms is well above a pump interval and far below the
// 120ms COMMIT_QUIET window which precedes every replay.
bool stagingInputPending()
{
    return inputPending() || Application::GetLastInputInterval() < 50;
}
std::size_t cost(const TileKey& key)
{
    const auto padded = pdffast::tileWithGutter(key);
    return std::size_t(padded.width) * padded.height * 4;
}
}

struct PdfFastDocument::Impl : public SfxListener
{
    struct Page
    {
        explicit Page(SdrGrafObj* graphic)
            : original(graphic)
        {
        }
        rtl::Reference<SdrGrafObj> original;
        bool prepared = false;
        bool failed = false;
        bool metadataReady = true;
        bool metadataFailed = false;
        sal_uInt64 pageId = 0;
        bool dirty = false;
        Size size;
        std::array<sal_Int32, 4> borders{};
        drawinglayer::primitive2d::Primitive2DContainer background;
        drawinglayer::primitive2d::Primitive2DContainer sourceContent;
        std::vector<drawinglayer::primitive2d::Primitive2DContainer> content;
        std::vector<SdrLayerID> layers;
        std::vector<std::pair<basegfx::B2DRectangle, OUString>> sourceLinks;
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
        // Navigation intent (SwitchPage): the gate stays Waiting (read-only
        // preview, no wait cursor) while the page converts. Explicit intents
        // (mode change, page click, UNO) use Preparing. Both convert after
        // the same short debounce and both are abandoned on input.
        bool settle = false;
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
    bool runningPrefetch = false;
    struct PrefetchedEdit
    {
        rtl::Reference<SdrGrafObj> original;
        // Transport spool only. The UI-thread replay happens on the commit
        // path, after input settles; it is never run speculatively.
        pdffast::NativeImport::Result result;
    };
    std::deque<PrefetchedEdit> prefetchedEdits;
    std::set<sal_uInt64> prefetchAttempted;
    // Incremental UI-thread commit. Staging happens once; object insertion is
    // sliced by COMMIT_SLICE and only runs while input is quiet, so a page
    // flip never waits for the complete replay. Staged objects are inserted
    // below the opaque PDF placeholder; removing the placeholder is the
    // final one-turn swap.
    struct Commit
    {
        rtl::Reference<SdrGrafObj> original;
        // Page identity only: the SdrPage may be deleted between slices.
        sal_uInt64 pageId = 0;
        Page* entry = nullptr;
        std::vector<rtl::Reference<SdrObject>> objects;
        std::size_t next = 0;
        sal_uInt32 originalPosition = 0;
        sal_uInt64 metadataUs = 0;
        sal_uInt64 stagingUs = 0;
        sal_uInt64 insertUs = 0;
        // Unlocking the model after a slice flushes deferred broadcasts and
        // reformats edge objects document-wide; it is slice overhead too.
        sal_uInt64 unlockUs = 0;
        sal_uInt64 maxSliceUs = 0;
        sal_uInt64 slices = 0;
        Clock::time_point started{};
    };
    std::optional<Commit> commit;
    bool startCommit(const rtl::Reference<SdrGrafObj>&, const pdffast::NativeImport::Result&);
    bool advanceCommit();
    bool insertCommitSlice(Commit&, bool unlimited);
    void finalizeCommit();
    void abortCommit(bool failure, const char* reason);
    void finishCommitNow();
    void completeCommitViews(const rtl::Reference<SdrGrafObj>&, sal_uInt64 pageId, bool ready);
    // Direction the user is traveling; the next request in that direction is
    // transported first.
    int navigationDirection = 1;
    int previousEditPage = -1;
    bool prefetchEdits();
    std::vector<rtl::Reference<SdrObject>> stagePageObjects(
        const rtl::Reference<SdrGrafObj>& original,
        const pdffast::NativeImport::Result& result, bool interruptible);
    std::optional<pdffast::NativeImport::Result> pendingEditResult;
    // Transport result whose UI replay was abandoned on user input. For
    // non-detached results its helper lease stays held, so the retry replays
    // the same spool instead of waiting for another transport round trip.
    std::optional<pdffast::NativeImport::Result> stagedResult;
    bool stagedLease = false;
    void dropStagedResult()
    {
        if (!stagedResult)
            return;
        stagedResult.reset();
        if (stagedLease)
        {
            stagedLease = false;
            importer->release();
        }
    }
    // Set by startCommit when staging was abandoned because input arrived.
    bool stagingInterrupted = false;
    Clock::time_point lastScroll{};
    Timer poll{ "Draw PDF tile delivery" };
    void wake()
    {
        // Sidebar retries run every 10 ms. Restarting a 16 ms timer on every
        // request would postpone delivery forever while thumbnails are pending.
        if (!poll.IsActive())
            poll.Start();
    }
    bool inputQuiet(std::chrono::milliseconds limit) const
    {
        return Clock::now() - lastScroll >= limit
               && Application::GetLastInputInterval() >= sal_uInt64(limit.count())
               && !Application::AnyInput(VclInputFlags::MOUSE | VclInputFlags::KEYBOARD);
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

    static auto borders(const SdrPage& page)
    {
        return std::array<sal_Int32, 4>{ page.GetLeftBorder(), page.GetUpperBorder(),
                                         page.GetRightBorder(), page.GetLowerBorder() };
    }

    static auto primitives(const SdrObject& object)
    {
        drawinglayer::primitive2d::Primitive2DContainer result;
        object.GetViewContact().getViewIndependentPrimitive2DContainer(result);
        return result;
    }

    static auto background(const SdrPage& page)
    {
        drawinglayer::primitive2d::Primitive2DContainer result;
        // Page fill and master descriptor, excluding the page's object hierarchy.
        auto& contact = page.GetViewContact();
        contact.GetViewContact(2).getViewIndependentPrimitive2DContainer(result);
        contact.GetViewContact(3).getViewIndependentPrimitive2DContainer(result);
        return result;
    }

    static void markDirty(Page& entry)
    {
        entry.dirty = true;
        // Keep the immutable baseline. An edit followed by a complete undo can
        // still be compared against it safely at export time.
    }

    void Notify(SfxBroadcaster&, const SfxHint& hint) override
    {
        // Import and metadata delivery are internal changes, not user edits.
        if (preparing || hint.GetId() != SfxHintId::ThisIsAnSdrHint)
            return;
        const auto& change = static_cast<const SdrHint&>(hint);
        switch (change.GetKind())
        {
            case SdrHintKind::ObjectChange:
            case SdrHintKind::ObjectInserted:
            case SdrHintKind::ObjectRemoved:
            {
                const auto* page = change.GetPage();
                for (auto& entry : pages)
                    if (!page || page->IsMasterPage() || entry.pageId == page->GetUniqueID())
                        markDirty(entry);
                break;
            }
            case SdrHintKind::LayerChange:
            case SdrHintKind::LayerOrderChange:
            case SdrHintKind::ModelCleared:
                for (auto& entry : pages)
                    markDirty(entry);
                break;
            default:
                break; // Selection, navigation and page order do not change content.
        }
    }

    void rememberContent(Page& entry, SdrPage& page)
    {
        entry.content.clear();
        entry.layers.clear();
        entry.sourceContent.clear();
        if (entry.dirty)
            return;
        for (size_t n = 0; n < page.GetObjCount(); ++n)
            entry.content.push_back(primitives(*page.GetObj(n)));
        for (size_t n = 0; n < page.GetObjCount(); ++n)
            entry.layers.push_back(page.GetObj(n)->GetLayer());
    }

    bool editPending(int page) const
    {
        if (commit && commit->original.is()
            && commit->original->GetGraphic().getPageNumber() == page)
            return true;
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
        prefetchedEdits.clear();
        prefetchAttempted.clear();
        dropStagedResult();
        // Leaving the edit mode drops a suspended commit too; the placeholder
        // is still intact, so the page returns to its read-only preview.
        abortCommit(false, "edit-mode-off");
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
            pages.emplace_back(original.get());
            auto& entry = pages.back();
            entry.pageId = page->GetUniqueID();
            entry.size = page->GetSize();
            entry.borders = borders(*page);
            entry.background = background(*page);
            entry.sourceLinks = page->getLinkAnnotations();
            if (original.is())
                entry.sourceContent = primitives(*original);
            entry.dirty = page->GetObjCount() != 1;
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
        StartListening(doc);
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
            pages.emplace_back(object);
            // No original edit baseline is available for a discovered clone.
            pages.back().dirty = true;
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
            entry->sourceLinks = result->GetLinksInfo();
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
        // The document resets this adapter before its pages die; staged
        // objects still inserted must leave the page with it, and the released
        // references must never outlive their page.
        abortCommit(false, "shutdown");
        EndListeningAll();
        poll.Stop();
        metadataIdle.Stop();
        prefetchedEdits.clear();
        importer.reset(); // join the producer before removing its queued UI event
        if (editEvent)
            Application::RemoveUserEvent(editEvent);
        session.reset(); // cancel and join before the model or original graphics die
    }

    rtl::Reference<SdrPage> importEditablePage(const Graphic& graphic,
                                               const pdffast::NativeImport::Result& result,
                                               bool interruptible)
    {
        using namespace css;
        // Staging runs in one UI-thread turn; keep every sub-step timed so a
        // page that feels slow can be attributed without a profiler. With
        // interruptible set (interactive commit), the recorded replay polls
        // for input and this function stops between its phases too.
        const auto micros = [](auto duration) {
            return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        };
        const auto link = graphic.GetSharedGfxLink();
        if (!link || graphic.getPageNumber() < 0)
            throw std::runtime_error("Missing native PDF page");
        const auto bytes = link->getDataContainer().getDataShared();
        if (!bytes || bytes->empty())
            throw std::runtime_error("Missing native PDF source");
        if (result.success && result.page != graphic.getPageNumber())
            throw std::runtime_error("Native PDF transport failed");

        // The asynchronous command transport is an optimization around the
        // native Draw importer. Complex pages can make that transport fail
        // after the helper has already produced partial output. Fall back to
        // the ordinary native filter from the frozen source instead of leaving
        // an otherwise editable page permanently read-only.
        std::unique_ptr<utl::TempFileNamed> fallbackSource;
        OUString inputUrl(u"private:stream"_ustr);
        uno::Sequence<beans::PropertyValue> fallbackProperties;
        if (!result.success)
        {
            fallbackSource = std::make_unique<utl::TempFileNamed>();
            fallbackSource->EnableKillingFile();
            SvFileStream stream(fallbackSource->GetURL(), StreamMode::WRITE);
            stream.WriteBytes(bytes->data(), bytes->size());
            stream.Close();
            if (stream.GetError())
                throw std::runtime_error("Cannot create native PDF fallback source");
            inputUrl = fallbackSource->GetURL();
            fallbackProperties = comphelper::InitPropertySequence({
                { "URL", uno::Any(inputUrl) },
                { "FilterName", uno::Any(u"draw_pdf_import"_ustr) },
                { "FilterOptions", uno::Any(u"LoongOfficePdfPage="_ustr
                                            + OUString::number(graphic.getPageNumber() + 1)) },
            });
            SAL_INFO("pdffast", "native-edit-fallback page=" << graphic.getPageNumber());
        }

        // Initialize an import/data model. Completing a new Draw document here
        // installs defaults such as automatic kerning which the normal PDF
        // loading path does not use, changing the imported text layout.
        auto* nativeShell
            = new DrawDocShell(SfxObjectCreateMode::EMBEDDED, true, DocumentType::Draw);
        SfxObjectShellLock shell(nativeShell);
        comphelper::ScopeGuard close([&] {
            // Destroying the temporary import model is part of the atomic
            // staging turn: thousands of imported objects are torn down here.
            const auto closeStart = Clock::now();
            nativeShell->DoClose();
            SAL_INFO("pdffast", "native-edit-close pdf_page="
                                    << graphic.getPageNumber() << " close_us="
                                    << micros(Clock::now() - closeStart));
        });
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
        uno::Sequence<beans::PropertyValue> replayProperties = comphelper::InitPropertySequence({
            { "URL", uno::Any(inputUrl) },
            { "LoongOfficePdfCommands", uno::Any(result.commands) },
            { "LoongOfficePdfBinary", uno::Any(result.binary) },
            { "FilterName", uno::Any(u"draw_pdf_import"_ustr) },
            { "FilterOptions", uno::Any(u"LoongOfficePdfPage="_ustr
                                        + OUString::number(graphic.getPageNumber() + 1)) },
        });
        if (interruptible)
        {
            // Private pdfimport switch: the replay polls for pending input and
            // abandons itself instead of making a page flip wait for it. The
            // property is never passed by ordinary PDF import.
            replayProperties.realloc(replayProperties.getLength() + 1);
            beans::PropertyValue property;
            property.Name = "LoongOfficePdfInterruptible";
            property.Value <<= true;
            replayProperties.getArray()[replayProperties.getLength() - 1] = property;
        }
        const auto started = Clock::now();
        const bool imported = result.success ? filter->filter(replayProperties)
                                             : filter->filter(fallbackProperties);
        // The replay already stopped on input; stop before the remaining
        // staging phases too instead of paying for objects nobody will use.
        if (interruptible && stagingInputPending())
            throw StagingInterrupted();
        auto& nativeDoc = *nativeShell->GetDoc();
        const auto count = nativeDoc.GetSdPageCount(PageKind::Standard);
        SAL_INFO("pdffast", "native-edit-import backend="
                                << (result.success ? "session" : "native-fallback")
                                << " pdf_page="
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
        const auto burnInStart = Clock::now();
        nativeDoc.BurnInStyleSheetAttributes();
        if (interruptible && stagingInputPending())
            throw StagingInterrupted(); // before cloning thousands of objects
        const auto burnInUs = micros(Clock::now() - burnInStart);
        const auto cloneStart = Clock::now();
        auto staged = nativeDoc.GetSdPage(0, PageKind::Standard)->CloneSdrPage(doc);
        const auto cloneUs = micros(Clock::now() - cloneStart);
        if (!staged.is())
            throw std::runtime_error("Cannot stage native PDF page objects");
        if (interruptible && stagingInputPending())
            throw StagingInterrupted();
        const auto flattenStart = Clock::now();
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
        const auto flattenUs = micros(Clock::now() - flattenStart);

        // Native page cloning preserves geometry, groups and connector links.
        // Map layer IDs by name, without Paste()'s recentering or layer override.
        const auto layerStart = Clock::now();
        auto& targetLayers = doc.GetLayerAdmin();
        const auto& sourceLayers = nativeDoc.GetLayerAdmin();
        std::map<SdrLayerID, SdrLayerID> layerIds;
        SdrObjListIter objects(staged.get(), SdrIterMode::DeepWithGroups);
        while (objects.IsMore())
        {
            auto* object = objects.Next();
            auto mapped = layerIds.find(object->GetLayer());
            if (mapped == layerIds.end())
            {
                const auto* layer = sourceLayers.GetLayerPerID(object->GetLayer());
                if (!layer)
                    throw std::runtime_error("Missing native PDF object layer");
                auto* target = targetLayers.GetLayer(layer->GetName());
                if (!target)
                    target = targetLayers.NewLayer(layer->GetName());
                mapped = layerIds.emplace(object->GetLayer(), target->GetID()).first;
            }
            // The staged page is standalone and not displayed: SetLayer would
            // broadcast one ObjectChange into the live document per object (and
            // create a ViewContact for each), which dominated staging on pages
            // with thousands of objects. Insertion broadcasts normally later.
            object->NbcSetLayer(mapped->second);
        }
        const auto layerUs = micros(Clock::now() - layerStart);
        SAL_INFO("pdffast", "native-edit-staging pdf_page="
                                << graphic.getPageNumber() << " burnin_us=" << burnInUs
                                << " clone_us=" << cloneUs << " flatten_us=" << flattenUs
                                << " layer_us=" << layerUs
                                << " objects=" << staged->GetObjCount());
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
        if (!preparing && !commit && !running.is() && intents.empty())
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
        if (!preparing && !commit && !running.is() && intents.empty())
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
    if (preparing || commit || running.is() || !intents.empty() || !session->idle())
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
    const auto count = m_impl->doc.GetSdPageCount(PageKind::Standard);
    std::unique_ptr<SfxProgress> progress;
    if (m_impl->doc.GetDocSh() && !Application::IsHeadlessModeEnabled()
        && std::any_of(m_impl->pages.begin(), m_impl->pages.end(),
                       [](const auto& page) { return !page.metadataReady; }))
        progress = std::make_unique<SfxProgress>(m_impl->doc.GetDocSh(),
                                                 SdResId(STR_PDF_EXPORT_PREPARING), count);
    for (sal_uInt16 n = 0; n < count; ++n)
    {
        if (!EnsureMetadata(*m_impl->doc.GetSdPage(n, PageKind::Standard)))
            return false;
        if (progress)
            progress->SetState(n + 1);
    }
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
    // Synchronous barrier: a suspended incremental commit is finished here
    // rather than raced by a second replay of the same page.
    m_impl->finishCommitNow();
    m_impl->preparePage(*pageView->GetPage(), &view);
}

void PdfFastDocument::SetEditMode(bool enabled) { m_impl->setEditMode(enabled); }

bool PdfFastDocument::IsEditMode() const { return m_impl->editMode; }

bool PdfFastDocument::HasPrefetchedPage(const SdrPage& page) const
{
    return std::any_of(
        m_impl->prefetchedEdits.begin(), m_impl->prefetchedEdits.end(),
        [&](const auto& cached) { return cached.original->getSdrPageFromSdrObject() == &page; });
}

void PdfFastDocument::NoteScroll() { m_impl->lastScroll = Clock::now(); }

void PdfFastDocument::RequestCurrentPage(SdrView& view, const Link<bool, void>& changed,
                                         bool settle)
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
    const int index = (int(pageView->GetPage()->GetPageNum()) - 1) / 2;
    if (impl.previousEditPage >= 0 && index != impl.previousEditPage)
        impl.navigationDirection = index > impl.previousEditPage ? 1 : -1;
    impl.previousEditPage = index;
    if (impl.runningPrefetch && impl.running.is()
        && impl.running->getSdrPageFromSdrObject() != pageView->GetPage())
    {
        if (auto* page = impl.running->getSdrPageFromSdrObject())
            impl.prefetchAttempted.erase(page->GetUniqueID());
        impl.importer->cancel();
    }
    impl.wake();
    auto* entry = impl.findPage(*pageView->GetPage());
    if (!entry || entry->prepared)
        return;
    entry->failed = false; // Revisiting the page is a retry.
    // Navigation keeps the page in its read-only preview (Waiting, no wait
    // cursor); an explicit edit action blocks it (Preparing). Both start
    // conversion after the same short debounce, and the replay abandons
    // itself as soon as input arrives, so browsing never waits for it.
    impl.setGate(view, settle ? EditState::Waiting : EditState::Preparing);
    impl.intents[&view] = { entry->original, Clock::now() + REQUEST_DEBOUNCE, settle };
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
    if (impl.gates.empty())
    {
        impl.prefetchedEdits.clear();
        impl.prefetchAttempted.clear();
        impl.dropStagedResult();
        if (impl.runningPrefetch)
            impl.importer->cancel();
    }
    impl.schedule();
}

bool PdfFastDocument::Impl::processEdits()
{
    // The private replay pumps the event loop while this thread is busy, so a
    // nested call (Deliver timer, EditCompleted) can arrive mid-staging. It
    // must not advance or abort the commit the outer frame is still building;
    // this is deferred until staging has finished and rescheduled then.
    if (preparing)
        return true;
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
    // A suspended incremental commit is advanced first; slices are bounded and
    // pause whenever input arrives, so flips never wait for the whole replay.
    if (commit)
    {
        // A request for a different page wins over the partially inserted one,
        // navigation included: the user is already looking elsewhere, so the
        // remaining slices must not delay that page's conversion. Its
        // placeholder is still complete, so dropping the staged objects keeps
        // the page valid and editing responsive.
        const bool superseded = std::any_of(intents.begin(), intents.end(), [&](const auto& intent) {
            return intent.second.original != commit->original;
        });
        if (!superseded)
        {
            advanceCommit();
            return true;
        }
        abortCommit(false, "superseded");
    }
    for (auto it = prefetchedEdits.begin(); it != prefetchedEdits.end();)
    {
        auto* page = it->original->getSdrPageFromSdrObject();
        const bool wanted = std::any_of(intents.begin(), intents.end(), [&](const auto& intent) {
            return intent.second.original == it->original;
        });
        const auto link = it->original->GetGraphic().GetSharedGfxLink();
        if (!page || &page->getSdrModelFromSdrPage() != &doc || !editMode || !link
            || link->getDataContainer().getDataShared() != source
            || it->result.page != it->original->GetGraphic().getPageNumber())
        {
            it = prefetchedEdits.erase(it);
            continue;
        }
        if (wanted)
        {
            if (inputQuiet(COMMIT_QUIET))
            {
                auto cached = std::move(*it);
                it = prefetchedEdits.erase(it);
                SAL_INFO("pdffast", "native-edit-prefetch-hit page=" << cached.result.page);
                // A speculative transport may still run for another page; it
                // must not hold the helper while this page commits.
                if (running.is() && running != cached.original)
                    importer->cancel();
                startCommit(cached.original, cached.result);
                if (stagingInterrupted)
                {
                    // Input abandoned the replay. The spool is detached, so no
                    // lease is held; the intent replays it once input settles.
                    dropStagedResult();
                    stagedResult = std::move(cached.result);
                    stagedLease = false;
                }
                schedule();
                return true;
            }
        }
        ++it;
    }
    if (running.is())
    {
        const bool wanted = std::any_of(intents.begin(), intents.end(), [&](const auto& intent) {
            return intent.second.original == running;
        });
        if (!wanted
            && (!runningPrefetch || !editMode || !intents.empty() || gates.empty()
                || (doc.GetDocSh()
                    && (doc.GetDocSh()->IsReadOnly() || doc.GetDocSh()->IsReadOnlyUI()))))
            importer->cancel();
        if (!pendingEditResult)
            pendingEditResult = importer->takeResult();
        if (pendingEditResult && wanted && pendingEditResult->success
            && !inputQuiet(COMMIT_QUIET))
            return true;
        if (pendingEditResult)
        {
            auto result = std::move(pendingEditResult);
            pendingEditResult.reset();
            auto original = running;
            running.clear();
            // The helper lease is released when the result is consumed, except
            // when input abandons the replay: then the spool is retained so
            // the retry does not need another transport round trip.
            bool keepLease = false;
            comphelper::ScopeGuard release([&] {
                if (!keepLease)
                    importer->release();
            });
            auto* page = original->getSdrPageFromSdrObject();
            const auto link = original->GetGraphic().GetSharedGfxLink();
            if (runningPrefetch && !wanted && editMode && intents.empty() && page
                && &page->getSdrModelFromSdrPage() == &doc && result->success && result->storage)
            {
                if (prefetchedEdits.size() == 2)
                    prefetchedEdits.pop_front();
                // Publishing the transport result must stay cheap. Replaying it
                // into Draw objects happens only on the commit path.
                SAL_INFO("pdffast", "native-edit-prefetch-ready page=" << result->page);
                prefetchedEdits.push_back(PrefetchedEdit{ original, std::move(*result) });
            }
            runningPrefetch = false;
            if (wanted && page && &page->getSdrModelFromSdrPage() == &doc && link
                && link->getDataContainer().getDataShared() == source && result->page == runningPage
                && result->page == original->GetGraphic().getPageNumber())
            {
                if (!result->success)
                    SAL_WARN("pdffast", "Native page transport failed, using Draw fallback: "
                                              << result->error);
                // A navigation gate stays Waiting until startCommit promotes
                // it: the wait cursor must track the actual UI-thread commit,
                // not the background replay.
                startCommit(original, *result);
                if (stagingInterrupted)
                {
                    dropStagedResult();
                    stagedResult = std::move(*result);
                    stagedLease = true;
                    keepLease = true;
                }
                original->ActionChanged();
                schedule();
                return true;
            }
            // Complete/fail only this intent; edits made elsewhere while the
            // worker ran keep their undo history and current modified state.
            for (auto it = intents.begin(); it != intents.end();)
                if (it->second.original == original)
                {
                    setGate(*it->first, PdfFastDocument::EditState::Failed);
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
    {
        // Drop a retained replay source that no pending intent targets any
        // more, releasing its helper lease so a fresh transport can start.
        if (stagedResult
            && std::none_of(intents.begin(), intents.end(), [&](const auto& intent) {
                   return intent.second.original->GetGraphic().getPageNumber()
                          == stagedResult->page;
               }))
            dropStagedResult();
        for (const auto& entry : intents)
        {
            const auto original = entry.second.original;
            const int page = original->GetGraphic().getPageNumber();
            if (stagedResult && stagedResult->page == page)
            {
                // The helper already produced this page; replay the retained
                // spool instead of asking for another transport round trip.
                // Wait for quiet so the retry is not interrupted again at once.
                if (inputQuiet(COMMIT_QUIET))
                {
                    auto result = std::move(*stagedResult);
                    stagedResult.reset();
                    stagingInterrupted = false;
                    startCommit(original, result);
                    if (stagingInterrupted)
                    {
                        // Keep the spool for the next attempt; a held lease
                        // (stagedLease) stays held too.
                        stagedResult = std::move(result);
                    }
                    else if (stagedLease)
                    {
                        stagedLease = false;
                        importer->release();
                    }
                    schedule();
                    return true;
                }
                continue;
            }
            if (Clock::now() >= entry.second.due
                && std::none_of(prefetchedEdits.begin(), prefetchedEdits.end(),
                                [&](const auto& cached) {
                                    return cached.original == entry.second.original;
                                }))
            {
                if (importer->request(page))
                {
                    running = original;
                    runningPage = page;
                    runningPrefetch = false;
                }
                break;
            }
        }
    }
    const bool background = prefetchEdits();
    return running.is() || !intents.empty() || background;
}

bool PdfFastDocument::Impl::prefetchEdits()
{
    if (!editMode || gates.empty() || preparing || commit
        || (doc.GetDocSh() && (doc.GetDocSh()->IsReadOnly() || doc.GetDocSh()->IsReadOnlyUI())))
        return false;
    if (running.is() || !intents.empty())
        return true;
    std::vector<rtl::Reference<SdrGrafObj>> candidates;
    for (const auto & [ view, gate ] : gates)
    {
        const auto* pv = view->GetSdrPageView();
        if (!pv || pv->GetPage()->IsMasterPage())
            continue;
        // Materialization removes the PDF placeholder; use the retained page
        // identity rather than looking for that object in the live page.
        if (std::none_of(pages.begin(), pages.end(), [&](const auto& entry) {
                return entry.pageId == pv->GetPage()->GetUniqueID() && entry.prepared;
            }))
            continue;
        const int index = (int(pv->GetPage()->GetPageNum()) - 1) / 2;
        for (int delta : { navigationDirection, -navigationDirection })
        {
            const int neighbor = index + delta;
            if (neighbor < 0 || neighbor >= doc.GetSdPageCount(PageKind::Standard))
                continue;
            auto* page = doc.GetSdPage(neighbor, PageKind::Standard);
            auto* entry = findPage(*page);
            if (entry && !entry->prepared && !entry->failed && entry->original.is()
                && page->GetObjCount() == 1 && page->GetObj(0) == entry->original.get())
                candidates.push_back(entry->original);
        }
    }
    std::erase_if(prefetchedEdits, [&](const auto& cached) {
        return std::find(candidates.begin(), candidates.end(), cached.original) == candidates.end();
    });
    std::erase_if(prefetchAttempted, [&](sal_uInt64 id) {
        return std::none_of(candidates.begin(), candidates.end(), [&](const auto& original) {
            const auto* page = original->getSdrPageFromSdrObject();
            return page && page->GetUniqueID() == id;
        });
    });
    for (const auto& original : candidates)
    {
        auto* page = original->getSdrPageFromSdrObject();
        if (!page || prefetchAttempted.contains(page->GetUniqueID())
            || std::any_of(prefetchedEdits.begin(), prefetchedEdits.end(),
                           [&](const auto& cached) { return cached.original == original; }))
            continue;
        // The transport runs in another process and only fills helper spools,
        // so it may start as soon as the input queue is empty. Its result is
        // replayed on the UI thread only after the commit gate opens.
        if (Application::AnyInput(VclInputFlags::MOUSE | VclInputFlags::KEYBOARD))
            return true;
        // One transport at a time. Foreground navigation cancels speculative
        // work; immutable results are bounded to two 32 MiB disk spools.
        if (importer->request(original->GetGraphic().getPageNumber(), true))
        {
            prefetchAttempted.insert(page->GetUniqueID());
            running = original;
            runningPage = original->GetGraphic().getPageNumber();
            runningPrefetch = true;
            SAL_INFO("pdffast", "native-edit-prefetch-start page=" << runningPage);
        }
        return running.is();
    }
    return false;
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
    // An unfinished incremental commit must be part of the saved document;
    // complete it before the full synchronous conversion below.
    m_impl->finishCommitNow();
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
    m_impl->prefetchedEdits.clear();
    m_impl->dropStagedResult();
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

const std::shared_ptr<const std::vector<sal_uInt8>>& PdfFastDocument::GetSourceData() const
{
    return m_impl->source;
}

const SdrGrafObj* PdfFastDocument::GetUnchangedPageGraphic(const SdrPage& page,
                                                           const SdrPageView& view)
{
    // Export sees a complete page: finish a suspended commit first.
    m_impl->finishCommitNow();
    // Notes still need native Draw annotation output. Unchanged links are copied
    // once by the existing external-page writer; shape replay is suppressed.
    const auto* sdPage = dynamic_cast<const SdPage*>(&page);
    if (!sdPage || !sdPage->getAnnotations().empty())
        return nullptr;
    auto& impl = *m_impl;
    for (auto& entry : impl.pages)
    {
        if (entry.pageId != page.GetUniqueID() || !entry.original.is())
            continue;
        if (!entry.metadataReady || sdPage->getLinkAnnotations() != entry.sourceLinks)
            return nullptr;
        if (!entry.prepared)
        {
            if (!entry.dirty && page.GetObjCount() == 1 && page.GetObj(0) == entry.original.get()
                && page.GetSize() == entry.size && Impl::borders(page) == entry.borders
                && Impl::background(page) == entry.background && entry.original->IsVisible()
                && entry.original->IsPrintable()
                && view.GetVisibleLayers().IsSet(entry.original->GetLayer())
                && view.GetPrintableLayers().IsSet(entry.original->GetLayer())
                && Impl::primitives(*entry.original) == entry.sourceContent)
                return entry.original.get();
            return nullptr;
        }
        if (entry.content.empty())
            return nullptr;
        // Events alone are insufficient (Nbc setters, page format and inherited
        // styles). Compare immutable primitives too. Never use document IsChanged:
        // another page may have changed, or a previous save cleared that flag.
        if (page.GetSize() != entry.size || Impl::borders(page) != entry.borders
            || Impl::background(page) != entry.background
            || page.GetObjCount() != entry.content.size())
            return nullptr;
        SdrObjListIter objects(&page, SdrIterMode::DeepWithGroups);
        while (objects.IsMore())
        {
            const auto* object = objects.Next();
            if (!object->IsVisible() || !object->IsPrintable()
                || !view.GetVisibleLayers().IsSet(object->GetLayer())
                || !view.GetPrintableLayers().IsSet(object->GetLayer()))
                return nullptr;
        }
        for (size_t n = 0; n < page.GetObjCount(); ++n)
        {
            const auto& object = *page.GetObj(n);
            if (object.GetLayer() != entry.layers[n] || !object.IsVisible() || !object.IsPrintable()
                || !view.GetVisibleLayers().IsSet(object.GetLayer())
                || !view.GetPrintableLayers().IsSet(object.GetLayer())
                || Impl::primitives(object) != entry.content[n])
                return nullptr;
        }
        SAL_INFO("pdffast", "native-export-source page="
                                << page.GetPageNum()
                                << " pdf_page=" << entry.original->GetGraphic().getPageNumber());
        return entry.original.get();
    }
    return nullptr;
}

void PdfFastDocument::ExportPageLinks(const SdrPage& page, vcl::PDFExtOutDevData& output) const
{
    const auto* sdPage = dynamic_cast<const SdPage*>(&page);
    if (!sdPage || sdPage->getLinkAnnotations().empty())
        return;
    const auto entry
        = std::find_if(m_impl->pages.begin(), m_impl->pages.end(),
                       [&](const auto& item) { return item.pageId == page.GetUniqueID(); });
    if (entry == m_impl->pages.end() || !entry->prepared || !entry->metadataReady
        || !entry->original.is())
        return;
    // An original page graphic inserted back into the live page already copies
    // its PDF annotations. Do not emit the retained metadata a second time.
    SdrObjListIter objects(&page, SdrIterMode::DeepWithGroups);
    while (objects.IsMore())
    {
        const auto* graphic = dynamic_cast<const SdrGrafObj*>(objects.Next());
        if (!graphic)
            continue;
        const auto& link = graphic->GetGraphic().GetSharedGfxLink();
        if (link && link->GetType() == GfxLinkType::NativePdf
            && link->getDataContainer().getDataShared() == m_impl->source
            && graphic->GetGraphic().getPageNumber()
                   == entry->original->GetGraphic().getPageNumber())
            return;
    }
    const auto coordinate = [](double value) {
        return tools::Long(
            std::lround(o3tl::convert(value, o3tl::Length::twip, o3tl::Length::mm100)));
    };
    for (const auto & [ rectangle, url ] : sdPage->getLinkAnnotations())
    {
        const tools::Rectangle area(
            Point(coordinate(rectangle.getMinX()), coordinate(rectangle.getMinY())),
            Point(coordinate(rectangle.getMaxX()), coordinate(rectangle.getMaxY())));
        const bool tagged = output.GetIsExportTaggedPDF();
        if (tagged)
            output.WrapBeginStructureElement(vcl::pdf::StructElement::Link);
        const sal_Int32 id = output.CreateLink(area, url);
        output.SetLinkURL(id, url);
        if (tagged)
        {
            output.SetAlternateText(url);
            output.SetStructureAttributeNumerical(vcl::pdf::PDFWriter::LinkAnnotation, id);
            output.EndStructureElement();
        }
    }
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

void PdfFastDocument::Impl::completeCommitViews(const rtl::Reference<SdrGrafObj>& original,
                                                sal_uInt64 pageId, bool ready)
{
    for (auto it = intents.begin(); it != intents.end();)
        if (it->second.original == original)
        {
            setGate(*it->first, ready ? PdfFastDocument::EditState::Ready
                                      : PdfFastDocument::EditState::Failed);
            it = intents.erase(it);
        }
        else
            ++it;
    // Views that navigated onto the committing page after its intent was
    // dropped still hold a read-only gate; release them with the page.
    for (auto & [ view, gate ] : gates)
    {
        auto* pageView = view->GetSdrPageView();
        if (pageView && pageView->GetPage()->GetUniqueID() == pageId
            && gate.state != PdfFastDocument::EditState::Ready)
            setGate(*view, ready ? PdfFastDocument::EditState::Ready
                                 : PdfFastDocument::EditState::Failed);
    }
}

bool PdfFastDocument::Impl::startCommit(const rtl::Reference<SdrGrafObj>& original,
                                        const pdffast::NativeImport::Result& result)
{
    stagingInterrupted = false;
    if (commit || !original.is())
        return false;
    auto* page = original->getSdrPageFromSdrObject();
    if (!page || &page->getSdrModelFromSdrPage() != &doc || page->IsMasterPage())
    {
        completeCommitViews(original, 0, false);
        return false;
    }
    auto* entry = findPage(*page);
    if (!entry || entry->prepared)
    {
        completeCommitViews(original, entry ? entry->pageId : page->GetUniqueID(), true);
        return false;
    }
    if (!dynamic_cast<SdPage*>(page))
    {
        entry->failed = true;
        completeCommitViews(original, entry->pageId, false);
        return false;
    }
    const auto started = Clock::now();
    const auto metadataStart = Clock::now();
    const bool metadataOk = ensureMetadata(*page);
    const auto metadataUs
        = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - metadataStart)
              .count();
    if (!metadataOk)
    {
        entry->failed = true;
        completeCommitViews(original, entry->pageId, false);
        SAL_WARN("pdffast", "Native page conversion skipped, metadata failed: page="
                                << original->GetGraphic().getPageNumber());
        return false;
    }
    // Staging (filter replay + clone) still runs in one UI-thread turn, but
    // the replay checks for pending input and abandons itself within a few
    // milliseconds; object insertion is sliced between event-loop turns.
    const bool wasPreparing = preparing;
    preparing = true;
    comphelper::ScopeGuard resetPreparing([&] { preparing = wasPreparing; });
    const bool changed = doc.IsChanged();
    const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
    const bool undo = doc.IsUndoEnabled();
    comphelper::ScopeGuard restore([&] {
        doc.EnableUndo(undo);
        doc.SetChanged(changed);
        if (auto* shell = doc.GetDocSh())
            shell->SetModified(modified);
    });
    doc.EnableUndo(false);
    // Catch silent changes to raw pages before capturing the editable baseline.
    if (page->GetObjCount() != 1 || primitives(*original) != entry->sourceContent)
        markDirty(*entry);
    const auto originalRect = original->GetLogicRect();
    SAL_INFO("pdffast",
             "native-edit-begin page="
                 << page->GetPageNum() << " pdf_page=" << original->GetGraphic().getPageNumber()
                 << " x=" << originalRect.Left() << " y=" << originalRect.Top()
                 << " width=" << originalRect.GetWidth() << " height=" << originalRect.GetHeight());
    // Other windows and pages retain their own raster demand during import.
    for (auto viewport = viewports.begin(); viewport != viewports.end();)
        if ((!viewport->second.native
             && viewport->second.preview.page == original->GetGraphic().getPageNumber())
            || std::any_of(viewport->second.adjacent.begin(), viewport->second.adjacent.end(),
                           [&](const TileKey& key) {
                               return key.page == original->GetGraphic().getPageNumber();
                           }))
            viewport = viewports.erase(viewport);
        else
            ++viewport;
    schedule();
    commit.emplace();
    commit->original = original;
    commit->pageId = entry->pageId;
    commit->entry = entry;
    commit->originalPosition = original->GetOrdNum();
    commit->metadataUs = metadataUs;
    commit->started = started;
    const auto stagingStart = Clock::now();
    bool staged = false;
    try
    {
        commit->objects = stagePageObjects(original, result, true);
        staged = true;
    }
    catch (const StagingInterrupted&)
    {
        stagingInterrupted = true;
    }
    catch (const css::uno::Exception& error)
    {
        SAL_WARN("pdffast", "Native page staging failed: " << error.Message);
    }
    catch (const std::exception& error)
    {
        SAL_WARN("pdffast", "Native page staging failed: " << error.what());
    }
    commit->stagingUs
        = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stagingStart)
              .count();
    if (!staged)
    {
        commit.reset();
        if (stagingInterrupted)
        {
            // User input arrived while the page was being replayed. Nothing is
            // broken: stay in the read-only preview without a wait cursor and
            // let the intent retry after the debounce. The transport result may
            // have been retained, so the retry need not replan everything.
            for (auto & [ view, intent ] : intents)
                if (intent.original == original)
                {
                    intent.due = Clock::now() + REQUEST_DEBOUNCE;
                    setGate(*view, PdfFastDocument::EditState::Waiting);
                }
            SAL_INFO("pdffast", "native-edit-interrupt pdf_page="
                                    << original->GetGraphic().getPageNumber());
            schedule();
            return false;
        }
        entry->failed = true;
        completeCommitViews(original, entry->pageId, false);
        schedule();
        return false;
    }
    // The staging page is gone once staging returns; rebind presentation
    // callbacks now instead of at the end, so no suspended slice ever holds a
    // dangling callback.
    const auto rebindStart = Clock::now();
    if (auto* stagedTarget = dynamic_cast<SdPage*>(page))
        for (const auto& object : commit->objects)
        {
            SdrObjListIter children(*object, SdrIterMode::DeepWithGroups);
            while (children.IsMore())
            {
                auto* child = children.Next();
                if (child->GetUserCall() && child->GetUserCall() != stagedTarget)
                    child->SetUserCall(stagedTarget);
            }
        }
    SAL_INFO("pdffast", "native-edit-rebind pdf_page="
                            << original->GetGraphic().getPageNumber()
                            << " objects=" << commit->objects.size() << " rebind_us="
                            << std::chrono::duration_cast<std::chrono::microseconds>(
                                   Clock::now() - rebindStart)
                                   .count());
    // Promote a Waiting navigation gate only now: the wait cursor must track
    // the actual UI-thread commit, not the background replay.
    for (auto & [ view, gate ] : gates)
    {
        auto* pageView = view->GetSdrPageView();
        if (pageView && pageView->GetPage()->GetUniqueID() == commit->pageId
            && gate.state != PdfFastDocument::EditState::Preparing)
            setGate(*view, PdfFastDocument::EditState::Preparing);
    }
    // The first slice runs now; input was quiet when the caller decided to
    // commit, and later slices pause as soon as the user interacts again.
    advanceCommit();
    return true;
}

bool PdfFastDocument::Impl::insertCommitSlice(Commit& current, bool unlimited)
{
    auto* page = current.original.is() ? current.original->getSdrPageFromSdrObject() : nullptr;
    if (!page || page->GetUniqueID() != current.pageId
        || &page->getSdrModelFromSdrPage() != &doc)
        return false;
    if (current.next >= current.objects.size())
        return true;
    const bool wasPreparing = preparing;
    preparing = true;
    comphelper::ScopeGuard resetPreparing([&] { preparing = wasPreparing; });
    const bool changed = doc.IsChanged();
    const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
    const bool undo = doc.IsUndoEnabled();
    const bool wasLocked = doc.isLocked();
    comphelper::ScopeGuard restore([&] {
        const auto unlockStart = Clock::now();
        doc.setLock(wasLocked);
        current.unlockUs
            += sal_uInt64(std::chrono::duration_cast<std::chrono::microseconds>(
                              Clock::now() - unlockStart)
                              .count());
        doc.EnableUndo(undo);
        doc.SetChanged(changed);
        if (auto* shell = doc.GetDocSh())
            shell->SetModified(modified);
    });
    doc.EnableUndo(false);
    doc.setLock(true);
    const auto started = Clock::now();
    try
    {
        while (current.next < current.objects.size())
        {
            // Insert below the opaque PDF placeholder: the visible page stays
            // the complete raster preview until the final one-turn swap.
            page->InsertObject(current.objects[current.next].get(),
                               current.originalPosition + current.next);
            ++current.next;
            if (!unlimited && Clock::now() - started >= COMMIT_SLICE)
                break;
        }
    }
    catch (const css::uno::Exception& error)
    {
        SAL_WARN("pdffast", "Native page insert failed: " << error.Message);
        return false;
    }
    catch (const std::exception& error)
    {
        SAL_WARN("pdffast", "Native page insert failed: " << error.what());
        return false;
    }
    const auto elapsed = sal_uInt64(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count());
    current.insertUs += elapsed;
    current.maxSliceUs = std::max(current.maxSliceUs, elapsed);
    ++current.slices;
    return true;
}

void PdfFastDocument::Impl::finalizeCommit()
{
    if (!commit)
        return;
    auto& current = *commit;
    auto* page = current.original.is() ? current.original->getSdrPageFromSdrObject() : nullptr;
    if (!page || page->GetUniqueID() != current.pageId
        || &page->getSdrModelFromSdrPage() != &doc)
    {
        abortCommit(true, "page-gone");
        return;
    }
    const bool wasPreparing = preparing;
    preparing = true;
    comphelper::ScopeGuard resetPreparing([&] { preparing = wasPreparing; });
    const bool changed = doc.IsChanged();
    const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
    const bool undo = doc.IsUndoEnabled();
    comphelper::ScopeGuard restore([&] {
        doc.EnableUndo(undo);
        doc.SetChanged(changed);
        if (auto* shell = doc.GetDocSh())
            shell->SetModified(modified);
    });
    doc.EnableUndo(false);
    // The one-turn reveal: the staged objects are complete and already sit
    // below the placeholder, so removing the raster handout is the swap. The
    // removal broadcasts like preparePage's; never suppress it with setLock.
    page->RemoveObject(current.original->GetOrdNum());
    current.entry->prepared = true; // A blank native page is a valid editable page too.
    const auto rememberStart = Clock::now();
    rememberContent(*current.entry, *page);
    const auto rememberUs
        = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - rememberStart)
              .count();
    // Page geometry, background and annotations already belong to the live
    // Draw document. Materialization replaces only the source graphic;
    // importing must preserve page-format changes and edits made elsewhere.
    page->GetViewContact().ActionChanged();
    const auto original = current.original;
    const auto pageId = current.pageId;
    const auto objectCount = page->GetObjCount();
    SAL_INFO("pdffast",
             "native-edit-prepare page="
                 << page->GetPageNum() << " success=1 objects=" << objectCount
                 << " metadata_us=" << current.metadataUs << " staging_us=" << current.stagingUs
                 << " insert_us=" << current.insertUs << " unlock_us=" << current.unlockUs
                 << " slices=" << current.slices << " max_slice_us=" << current.maxSliceUs
                 << " remember_us=" << rememberUs
                 << " total_us="
                 << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()
                                                                          - current.started)
                        .count());
    commit.reset();
    completeCommitViews(original, pageId, true);
    schedule();
}

void PdfFastDocument::Impl::abortCommit(bool failure, const char* reason)
{
    if (!commit)
        return;
    auto current = std::move(*commit);
    commit.reset();
    auto* page = current.original.is() ? current.original->getSdrPageFromSdrObject() : nullptr;
    const bool valid = page && page->GetUniqueID() == current.pageId
                       && &page->getSdrModelFromSdrPage() == &doc;
    if (current.next)
    {
        // The placeholder was never removed: drop the partially inserted
        // objects and leave the page as it was before the commit.
        const bool wasPreparing = preparing;
        preparing = true;
        comphelper::ScopeGuard resetPreparing([&] { preparing = wasPreparing; });
        const bool changed = doc.IsChanged();
        const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
        const bool undo = doc.IsUndoEnabled();
        comphelper::ScopeGuard restore([&] {
            doc.EnableUndo(undo);
            doc.SetChanged(changed);
            if (auto* shell = doc.GetDocSh())
                shell->SetModified(modified);
        });
        doc.EnableUndo(false);
        if (valid)
        {
            for (auto object = current.objects.rbegin(); object != current.objects.rend();
                 ++object)
                if ((*object)->getSdrPageFromSdrObject() == page)
                    page->NbcRemoveObject((*object)->GetOrdNum());
            page->GetViewContact().ActionChanged();
        }
        else
        {
            // The page died with the staged objects it contained; anything it
            // never received must not keep a callback into it.
            for (const auto& object : current.objects)
                if (object->GetUserCall())
                    object->SetUserCall(nullptr);
        }
    }
    if (current.entry && failure)
        current.entry->failed = true;
    SAL_INFO("pdffast", "native-edit-abort pdf_page="
                            << current.original->GetGraphic().getPageNumber()
                            << " inserted=" << current.next << " failed=" << failure
                            << " reason=" << reason);
    if (failure)
        completeCommitViews(current.original, current.pageId, false);
    schedule();
}

void PdfFastDocument::Impl::finishCommitNow()
{
    if (!commit)
        return;
    if (!insertCommitSlice(*commit, true))
    {
        abortCommit(true, "sync-insert-failed");
        return;
    }
    finalizeCommit();
}

bool PdfFastDocument::Impl::advanceCommit()
{
    if (!commit)
        return false;
    auto& current = *commit;
    auto* page = current.original.is() ? current.original->getSdrPageFromSdrObject() : nullptr;
    // The page may have been deleted by the slide sorter while the commit was
    // suspended between slices; never touch a stale page pointer.
    if (!page || page->GetUniqueID() != current.pageId
        || &page->getSdrModelFromSdrPage() != &doc)
    {
        abortCommit(true, "page-gone");
        return false;
    }
    if (current.next < current.objects.size() && !inputQuiet(COMMIT_SLICE))
        return true; // Input has priority; the next poll tick resumes.
    if (!insertCommitSlice(current, false))
    {
        abortCommit(true, "insert-failed");
        return false;
    }
    if (current.next == current.objects.size())
        finalizeCommit();
    return true;
}

std::vector<rtl::Reference<SdrObject>> PdfFastDocument::Impl::stagePageObjects(
    const rtl::Reference<SdrGrafObj>& original, const pdffast::NativeImport::Result& result,
    bool interruptible)
{
    const bool changed = doc.IsChanged();
    const bool modified = doc.GetDocSh() && doc.GetDocSh()->IsModified();
    const bool undo = doc.IsUndoEnabled();
    comphelper::ScopeGuard restore([&] {
        doc.EnableUndo(undo);
        doc.SetChanged(changed);
        if (auto* shell = doc.GetDocSh())
            shell->SetModified(modified);
    });
    doc.EnableUndo(false);
    auto staged = importEditablePage(original->GetGraphic(), result, interruptible);
    if (!staged)
        throw std::runtime_error("Missing staged PDF page");
    const auto unlinkStart = Clock::now();
    std::vector<rtl::Reference<SdrObject>> objects;
    while (staged->GetObjCount())
        objects.push_back(staged->NbcRemoveObject(staged->GetObjCount() - 1));
    std::reverse(objects.begin(), objects.end());
    SAL_INFO("pdffast", "native-edit-unlink pdf_page="
                            << original->GetGraphic().getPageNumber()
                            << " objects=" << objects.size() << " unlink_us="
                            << std::chrono::duration_cast<std::chrono::microseconds>(
                                   Clock::now() - unlinkStart)
                                   .count());
    return objects;
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
    const auto metadataStart = Clock::now();
    if (!impl.ensureMetadata(target))
        return false;
    const auto metadataUs
        = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - metadataStart)
              .count();
    if (!completed)
    {
        // Explicit save/tests retain a synchronous barrier. Normal navigation
        // uses RequestCurrentPage and never waits for helper work on the UI.
        // A retained replay source holds the helper lease; release it first.
        impl.dropStagedResult();
        if (impl.running.is())
        {
            impl.importer->cancel();
            if (impl.pendingEditResult)
                impl.pendingEditResult.reset();
            else
                impl.importer->wait();
            impl.importer->release();
            impl.running.clear();
            impl.runningPrefetch = false;
        }
        if (!impl.importer->request(it->original->GetGraphic().getPageNumber()))
            return false;
        auto result = impl.importer->wait();
        comphelper::ScopeGuard release([&] { impl.importer->release(); });
        if (!result.success)
        {
            it->failed = true;
            SAL_WARN("pdffast", "Native single-page transport failed: " << result.error);
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
    // Catch silent changes to raw pages before capturing the editable baseline.
    if (page->GetObjCount() != 1 || primitives(*it->original) != it->sourceContent)
        markDirty(*it);
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
    // Stage/insert/rememberContent are the remaining UI-thread costs of a
    // commit; keep them separately visible so scheduling stays measurable.
    sal_uInt64 stagingUs = 0, insertUs = 0, rememberUs = 0;
    try
    {
        auto* targetPage = dynamic_cast<SdPage*>(page);
        if (!targetPage)
            throw std::runtime_error("Missing native Draw page");
        const auto originalPosition = it->original->GetOrdNum();
        const auto stagingStart = Clock::now();
        // Synchronous barrier (save/tests): keep the replay atomic here so a
        // stray mouse movement cannot abort a save or the public API.
        std::vector<rtl::Reference<SdrObject>> objects
            = impl.stagePageObjects(it->original, *completed, false);
        stagingUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stagingStart)
                        .count();
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
        {
            // Batch insertion: suppress the per-object model broadcast (UNO
            // shape events, repaint marks) like the SD import filters do via
            // setLock. Large pages otherwise pay one broadcast per object;
            // the page-level ActionChanged below delivers one update.
            const bool wasLocked = impl.doc.isLocked();
            impl.doc.setLock(true);
            comphelper::ScopeGuard unlock([&] { impl.doc.setLock(wasLocked); });
            std::size_t position = originalPosition;
            for (const auto& object : objects)
                page->InsertObject(object.get(), position++);
        }
        // SdPage cloning rebinds presentation callbacks to the staging page.
        // Moving its objects must not leave that temporary page as a callback.
        SdrObjListIter callbacks(page, SdrIterMode::DeepWithGroups);
        while (callbacks.IsMore())
        {
            auto* object = callbacks.Next();
            if (object->GetUserCall() && object->GetUserCall() != targetPage)
                object->SetUserCall(targetPage);
        }
        page->RemoveObject(it->original->GetOrdNum());
        committed = true;
        it->prepared = true; // A blank native page is a valid editable page too.
        insertUs
            = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stagingStart)
                  .count()
              - stagingUs;
        const auto rememberStart = Clock::now();
        impl.rememberContent(*it, *page);
        rememberUs = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()
                                                                          - rememberStart)
                         .count();
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
                 << " objects=" << page->GetObjCount()
                 << " metadata_us=" << metadataUs << " staging_us=" << stagingUs
                 << " insert_us=" << insertUs << " remember_us=" << rememberUs << " total_us="
                 << std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started)
                        .count());
    return it->prepared;
}
}
