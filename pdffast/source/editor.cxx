/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include "editor.hxx"
#include "input.hxx"
#include "layout.hxx"
#include "cache.hxx"
#include <osl/file.hxx>
#include <comphelper/scopeguard.hxx>
#include <svx/svdmodel.hxx>
#include <svl/lstner.hxx>
#include <svx/svdpage.hxx>
#include <svx/svdograf.hxx>
#include <svx/svdview.hxx>
#include <svx/svdmrkv.hxx>
#include <vcl/window.hxx>
#include <vcl/virdev.hxx>
#include <vcl/BitmapReadAccess.hxx>
#include <vcl/gdimtf.hxx>
#include <vcl/graph.hxx>
#include <vcl/gfxlink.hxx>
#include <vcl/pdfwriter.hxx>
#include <vcl/pdfextoutdevdata.hxx>
#include <vcl/filter/PDFiumLibrary.hxx>
#include <vcl/commandevent.hxx>
#include <vcl/event.hxx>
#include <vcl/ptrstyle.hxx>
#include <vcl/keycodes.hxx>
#include <vcl/svapp.hxx>
#include <vcl/region.hxx>
#include <vcl/wall.hxx>
#include <unotools/tempfile.hxx>
#include <tools/fract.hxx>
#include <tools/stream.hxx>
#include <map>
#include <stdexcept>

namespace pdffast
{
namespace
{
Size logicalSize(PageSize size)
{
    if (!size.valid())
        throw std::runtime_error("Invalid editable page size");
    return Size(std::max<tools::Long>(1, std::lround(size.width * 2540 / 72)),
                std::max<tools::Long>(1, std::lround(size.height * 2540 / 72)));
}

class PageView final : public SdrView
{
public:
    PageView(SdrModel& model, OutputDevice* device)
        : SdrView(model, device)
    {
    }
    void transformed() { TheresNewMapMode(); }
};

// Each prepared source page owns its model and undo history. It is committed by
// publishing the whole window, never by importing into a shared live document.
// Shared across prepared pages; models/history never enter the raster budget.
using SnapshotKey = std::tuple<int, std::uint64_t, tools::Long, tools::Long>;
using SnapshotCache = Cache<SnapshotKey, Bitmap>;

class ModelListener final : public SfxListener
{
    std::function<void()> m_changed;

public:
    ModelListener(SdrModel& model, std::function<void()> changed)
        : m_changed(std::move(changed))
    {
        StartListening(model);
    }
    void Notify(SfxBroadcaster&, const SfxHint& hint) override
    {
        const auto* drawing = dynamic_cast<const SdrHint*>(&hint);
        if (drawing && drawing->GetKind() != SdrHintKind::RefDeviceChange
            && drawing->GetKind() != SdrHintKind::BeginEdit)
            m_changed();
    }
};

class PageWindow final : public vcl::Window
{
    EditorHost m_host;
    int m_sourcePage;
    std::uint64_t m_revision = 0;
    SnapshotCache& m_snapshots;
    std::shared_ptr<Source> m_source;
    std::unique_ptr<SdrModel> m_model;
    rtl::Reference<SdrPage> m_page;
    std::unique_ptr<PageView> m_view;
    std::unique_ptr<ModelListener> m_listener;
    Size m_fullPixels;
    Point m_clipOffset;
    bool m_readOnly = false;
    bool m_zoomSettling = false;

    void invalidateSnapshot()
    {
        ++m_revision; // Conservative content epoch, not a count of user edits.
        m_snapshots.eraseIf(
            [&](const SnapshotKey& key) { return std::get<0>(key) == m_sourcePage; });
    }
    void notify()
    {
        invalidateSnapshot();
        const auto changed = m_host.changed;
        if (changed)
            changed();
    }
    void forwardKey(const KeyEvent& event)
    {
        // Opening another document can dispose this window and its owner. Keep
        // the callable alive independently, and never access members afterwards.
        const auto key = m_host.key;
        if (key)
            key(event);
    }
    bool paintSnapshot(vcl::RenderContext& context)
    {
        if (m_fullPixels.Width() <= 0 || m_fullPixels.Height() <= 0)
            return false;
        const double factor
            = std::min(1.0, 2048.0 / std::max(m_fullPixels.Width(), m_fullPixels.Height()));
        const Size pixels(std::max<tools::Long>(1, std::lround(m_fullPixels.Width() * factor)),
                          std::max<tools::Long>(1, std::lround(m_fullPixels.Height() * factor)));
        const SnapshotKey key{ m_sourcePage, m_revision, pixels.Width(), pixels.Height() };
        if (!m_snapshots.contains(key) && m_zoomSettling)
        {
            const auto previous = m_snapshots.keys([&](const SnapshotKey& candidate) {
                return std::get<0>(candidate) == m_sourcePage
                       && std::get<1>(candidate) == m_revision;
            });
            if (!previous.empty())
                if (const auto* bitmap = m_snapshots.get(previous.front()))
                {
                    context.DrawBitmap(Point(), m_page->GetSize(), *bitmap);
                    return true;
                }
        }
        if (!m_snapshots.contains(key))
        {
            // SdrModel/VCL stay on the UI thread. First snapshot creation is
            // synchronous; subsequent scroll paints only scale/blit the bitmap.
            std::lock_guard guard(m_source->engineMutex);
            ScopedVclPtrInstance<VirtualDevice> device;
            if (!device->SetOutputSizePixel(pixels))
                return false;
            const Size base = device->LogicToPixel(m_page->GetSize(), MapMode(MapUnit::Map100thMM));
            if (base.Width() <= 0 || base.Height() <= 0)
                return false;
            MapMode map(MapUnit::Map100thMM);
            map.SetScaleX(Fraction(pixels.Width(), base.Width()));
            map.SetScaleY(Fraction(pixels.Height(), base.Height()));
            device->SetMapMode(map);
            device->SetBackground(Wallpaper(COL_WHITE));
            device->Erase();
            paintForExport(*device);
            device->SetMapMode(MapMode(MapUnit::MapPixel));
            Bitmap bitmap = device->GetBitmap(Point(), pixels);
            {
                BitmapScopedReadAccess access(bitmap);
                if (!access)
                    return false;
            }
            m_snapshots.put(key, std::move(bitmap),
                            std::size_t(pixels.Width()) * pixels.Height() * 4);
        }
        if (const auto* bitmap = m_snapshots.get(key))
        {
            context.DrawBitmap(Point(), m_page->GetSize(), *bitmap);
            return true;
        }
        return false;
    }

public:
    PageWindow(vcl::Window* parent, PageSize size, const EditorHost& host, int sourcePage,
               SnapshotCache& snapshots, const std::shared_ptr<Source>& source)
        : Window(parent, WB_CLIPCHILDREN)
        , m_host(host)
        , m_sourcePage(sourcePage)
        , m_snapshots(snapshots)
        , m_source(source)
        , m_model(std::make_unique<SdrModel>(nullptr, nullptr, true))
    {
        SetBackground(Wallpaper(COL_WHITE));
        SetMapMode(MapMode(MapUnit::Map100thMM));
        m_model->SetScaleUnit(MapUnit::Map100thMM);
        m_model->SetMaxUndoActionCount(256);
        m_page = new SdrPage(*m_model);
        m_page->SetSize(logicalSize(size));
        m_page->SetBorder(0, 0, 0, 0);
        m_model->InsertPage(m_page.get());
        m_view = std::make_unique<PageView>(*m_model, GetOutDev());
        m_view->SetPageDecorationAllowed(false);
        m_view->SetMasterPageVisualizationAllowed(false);
        m_view->SetEditMode();
        m_view->SetFrameDragSingles();
        m_view->ShowSdrPage(m_page.get());
        m_listener = std::make_unique<ModelListener>(*m_model, [this] {
            invalidateSnapshot();
            Invalidate();
        });
    }
    virtual ~PageWindow() override { disposeOnce(); }
    void dispose() override
    {
        finish();
        m_listener.reset();
        m_view.reset();
        m_page.clear();
        m_model.reset();
        Window::dispose();
    }
    void import(const Graphic& graphic)
    {
        rtl::Reference<SdrGrafObj> original
            = new SdrGrafObj(*m_model, graphic, tools::Rectangle(Point(), m_page->GetSize()));
        m_page->InsertObject(original.get());
        m_model->EnableUndo(false);
        m_view->MarkObj(original.get(), m_view->GetSdrPageView());
        m_view->DoImportMarkedMtf();
        // A zero-object/unsupported conversion leaves the PDF graphic in place.
        // Never silently claim that an unconverted preview is editable.
        for (std::size_t i = 0; i < m_page->GetObjCount(); ++i)
            if (m_page->GetObj(i) == original.get())
                throw std::runtime_error("This page could not be converted to editable objects");
        m_view->UnmarkAllObj();
        m_model->ClearUndoBuffer();
        m_model->EnableUndo(true);
        m_model->SetChanged(false);
    }
    void finish()
    {
        if (m_view)
        {
            if (m_view->IsTextEdit() || m_view->IsAction())
                invalidateSnapshot();
            if (m_view->IsTextEdit())
                m_view->SdrEndTextEdit();
            if (m_view->IsAction())
                m_view->BrkAction();
        }
        if (IsMouseCaptured())
            ReleaseMouse();
    }
    bool modified() const { return m_model && m_model->IsChanged(); }
    void readOnly(bool enabled, bool zoomSettling)
    {
        if (m_zoomSettling != zoomSettling)
        {
            m_zoomSettling = zoomSettling;
            Invalidate();
        }
        if (enabled == m_readOnly)
            return;
        m_readOnly = enabled;
        if (enabled)
        {
            finish();
            m_view->UnmarkAllObj();
        }
        Invalidate();
    }
    void place(const tools::Rectangle& full, const tools::Rectangle& clipped)
    {
        const Point offset = clipped.TopLeft() - full.TopLeft();
        if (m_fullPixels != full.GetSize() || m_clipOffset != offset)
        {
            finish();
            m_fullPixels = full.GetSize();
            m_clipOffset = offset;
            SetPosSizePixel(clipped.TopLeft(), clipped.GetSize());
            Resize();
        }
        else
            SetPosSizePixel(clipped.TopLeft(), clipped.GetSize());
    }
    void undo(bool redo)
    {
        finish();
        m_view->UnmarkAllObj();
        if (redo)
            m_model->Redo();
        else
            m_model->Undo();
        Invalidate();
        notify();
    }
    void paintForExport(OutputDevice& device)
    {
        SdrView view(*m_model, &device);
        view.SetPageDecorationAllowed(false);
        view.SetMasterPageVisualizationAllowed(false);
        view.ShowSdrPage(m_page.get());
        view.CompleteRedraw(&device, vcl::Region(tools::Rectangle(Point(), m_page->GetSize())));
        view.HideSdrPage();
    }
    void Resize() override
    {
        if (!m_view)
            return;
        finish();
        const Size pixels = m_fullPixels;
        const Size base = LogicToPixel(m_page->GetSize(), MapMode(MapUnit::Map100thMM));
        if (pixels.Width() > 0 && pixels.Height() > 0 && base.Width() > 0 && base.Height() > 0)
        {
            MapMode map(MapUnit::Map100thMM);
            map.SetScaleX(Fraction(pixels.Width(), base.Width()));
            map.SetScaleY(Fraction(pixels.Height(), base.Height()));
            map.SetOrigin(PixelToLogic(Point(-m_clipOffset.X(), -m_clipOffset.Y()), map));
            SetMapMode(map);
            m_view->transformed();
        }
        Invalidate();
    }
    void Paint(vcl::RenderContext& context, const tools::Rectangle& rect) override
    {
        context.SetMapMode(GetMapMode());
        context.SetFillColor(COL_WHITE);
        context.SetLineColor();
        context.DrawRect(rect);
        if (!m_readOnly || !paintSnapshot(context))
            m_view->CompleteRedraw(&context, vcl::Region(rect));
    }
    void MouseButtonDown(const MouseEvent& event) override
    {
        if (m_readOnly)
        {
            GetParent()->GrabFocus();
            return;
        }
        GrabFocus();
        m_view->MouseButtonDown(event, GetOutDev());
        if (m_view->IsAction())
            CaptureMouse();
        SetPointer(m_view->GetPreferredPointer(PixelToLogic(event.GetPosPixel()), GetOutDev()));
        notify();
    }
    void MouseMove(const MouseEvent& event) override
    {
        if (m_readOnly)
        {
            SetPointer(PointerStyle::Arrow);
            return;
        }
        m_view->MouseMove(event, GetOutDev());
        SetPointer(m_view->GetPreferredPointer(PixelToLogic(event.GetPosPixel()), GetOutDev()));
    }
    void MouseButtonUp(const MouseEvent& event) override
    {
        if (m_readOnly)
            return;
        m_view->MouseButtonUp(event, GetOutDev());
        ReleaseMouse();
        notify();
    }
    void KeyInput(const KeyEvent& event) override
    {
        if (m_readOnly)
        {
            forwardKey(event);
            return;
        }
        const auto& key = event.GetKeyCode();
        if (key.GetCode() == KEY_ESCAPE)
        {
            // Give an active text edit/gesture its normal cancel operation.
            // An idle SdrView also consumes Escape, so route that case directly
            // to the reader instead of relying on KeyInput() returning false.
            if (m_view->IsTextEdit() || m_view->IsAction())
                m_view->KeyInput(event, this);
            else
            {
                forwardKey(event);
                return;
            }
            notify();
            return;
        }
        if (key.IsMod1() && (key.GetCode() == KEY_Z || key.GetCode() == KEY_Y))
        {
            undo(key.GetCode() == KEY_Y || key.IsShift());
            return;
        }
        if (key.IsMod1()
            && (key.GetCode() == KEY_O || key.GetCode() == KEY_S || key.GetCode() == KEY_ADD
                || key.GetCode() == KEY_SUBTRACT))
        {
            forwardKey(event);
            return;
        }
        if (!m_view->KeyInput(event, this))
        {
            forwardKey(event);
            return;
        }
        notify();
    }
    void Command(const CommandEvent& event) override
    {
        if (m_readOnly || event.GetCommand() == CommandEventId::Wheel)
        {
            const auto command = m_host.command;
            if (command)
                command(event);
            return; // Navigation does not invalidate the content snapshot.
        }
        m_view->Command(event, this);
        notify();
    }
};

class NativeEditor final : public Editor
{
    VclPtr<vcl::Window> m_parent;
    std::shared_ptr<Source> m_source;
    EditorHost m_host;
    std::shared_ptr<GfxLink> m_graphics;
    SnapshotCache m_snapshots{ 32 * 1024 * 1024 };
    std::map<int, VclPtr<PageWindow>> m_pages;
    int m_activePage = -1;

public:
    NativeEditor(vcl::Window* parent, const std::shared_ptr<Source>& source, const EditorHost& host)
        : m_parent(parent)
        , m_source(source)
        , m_host(host)
    {
        std::lock_guard guard(source->engineMutex);
        materializeSource(*source);
        m_graphics = std::make_shared<GfxLink>(source->bytes, GfxLinkType::NativePdf);
    }
    ~NativeEditor() override
    {
        for (auto & [ page, window ] : m_pages)
            window.disposeAndClear();
    }
    bool prepare(int page, PageSize size, OUString& error) override
    {
        if (contains(page))
            return true;
        VclPtr<PageWindow> candidate;
        try
        {
            // PDFium conversion and font collection are synchronous, UI-thread
            // operations in the existing API. Do not move SdrModel/VCL to a worker.
            std::lock_guard guard(m_source->engineMutex);
            EditorHost host = m_host;
            host.changed = [this, page] {
                m_activePage = page;
                if (m_host.changed)
                    m_host.changed();
            };
            candidate
                = VclPtr<PageWindow>::Create(m_parent, size, host, page, m_snapshots, m_source);
            candidate->import(Graphic(m_graphics, page));
            m_pages.emplace(page, candidate);
            return true;
        }
        catch (const std::exception& exception)
        {
            error = OUString::fromUtf8(exception.what());
        }
        catch (...)
        {
            error = u"Cannot prepare this page for editing"_ustr;
        }
        candidate.disposeAndClear();
        return false;
    }
    bool contains(int page) const override { return m_pages.find(page) != m_pages.end(); }
    void readOnly(bool enabled, bool zoomSettling) override
    {
        for (auto & [ page, window ] : m_pages)
            window->readOnly(enabled, zoomSettling);
    }
    void place(int page, const tools::Rectangle& rect, bool visible) override
    {
        auto it = m_pages.find(page);
        if (it == m_pages.end())
            return;
        if (!visible)
        {
            it->second->finish();
            it->second->Hide();
            return;
        }
        const auto clipped
            = rect.GetIntersection(tools::Rectangle(Point(), m_parent->GetOutputSizePixel()));
        if (clipped.IsEmpty())
        {
            it->second->finish();
            it->second->Hide();
            return;
        }
        it->second->place(rect, clipped);
        it->second->Show();
    }
    void focus(int page) override
    {
        if (!contains(page))
            return;
        finish();
        m_activePage = page;
        m_pages.at(page)->GrabFocus();
    }
    void finish() override
    {
        for (auto & [ page, window ] : m_pages)
            window->finish();
    }
    bool modified() const override
    {
        for (const auto & [ page, window ] : m_pages)
            if (window->modified())
                return true;
        return false;
    }
    void undo(bool redo) override
    {
        if (contains(m_activePage))
            m_pages.at(m_activePage)->undo(redo);
    }
    bool exportCopy(const OUString& url, OUString& error) override;
};

bool NativeEditor::exportCopy(const OUString& url, OUString& error)
{
    try
    {
        finish();
        std::lock_guard guard(m_source->engineMutex);
        auto engine = vcl::pdf::PDFiumLibrary::get();
        auto document = engine ? engine->openDocument(m_source->bytes.getData(),
                                                      m_source->bytes.getSize(), OString())
                               : nullptr;
        if (!document)
            throw std::runtime_error("Cannot open the source PDF for export");
        utl::TempFileNamed temp;
        temp.EnableKillingFile();
        if (!temp.IsValid())
            throw std::runtime_error("Cannot create an export file");
        {
            vcl::pdf::PDFWriter::PDFWriterContext context;
            context.URL = temp.GetURL();
            context.DocumentInfo.Title = u"Edited PDF"_ustr;
            vcl::pdf::PDFWriter writer(context, {});
            ScopedVclPtrInstance<VirtualDevice> device;
            device->EnableOutput(false);
            for (int page = 0; page < document->getPageCount(); ++page)
            {
                const auto points = document->getPageSize(page);
                const Size size = logicalSize({ points.getWidth(), points.getHeight() });
                device->SetMapMode(MapMode(MapUnit::Map100thMM));
                const Size pixels = device->LogicToPixel(size);
                if (pixels.Width() <= 0 || pixels.Height() <= 0 || pixels.Width() > 32768
                    || pixels.Height() > 32768
                    || std::uint64_t(pixels.Width()) * pixels.Height() > 64 * 1024 * 1024
                    || !device->SetOutputSizePixel(pixels))
                    throw std::runtime_error("The page is too large for PDF export");
                vcl::PDFExtOutDevData extra(*device);
                extra.SetCurrentPageNumber(page);
                extra.SetIsLosslessCompression(true);
                device->SetExtOutDevData(&extra);
                comphelper::ScopeGuard clearExtra([&] { device->SetExtOutDevData(nullptr); });
                GDIMetaFile metafile;
                metafile.Record(device);
                device->SetFillColor(COL_WHITE);
                device->SetLineColor();
                device->DrawRect(tools::Rectangle(Point(), size));
                auto edited = m_pages.find(page);
                if (edited != m_pages.end())
                    edited->second->paintForExport(*device);
                else
                {
                    const Graphic graphic(m_graphics, page);
                    const tools::Rectangle rect(Point(), size);
                    extra.BeginGroup();
                    graphic.Draw(*device, Point(), size);
                    extra.EndGroup(graphic, 0, rect, rect);
                }
                metafile.Stop();
                metafile.SetPrefMapMode(MapMode(MapUnit::Map100thMM));
                metafile.SetPrefSize(size);
                writer.NewPage(points.getWidth(), points.getHeight());
                writer.SetMapMode(metafile.GetPrefMapMode());
                vcl::pdf::PDFWriter::PlayMetafileContext play;
                play.m_bOnlyLosslessCompression = true;
                writer.PlayMetafile(metafile, play, &extra);
                extra.ResetSyncData(nullptr);
                device->SetExtOutDevData(nullptr);
            }
            if (!writer.Emit())
                throw std::runtime_error("Cannot finish PDF export");
        }
        // Exclusive create: never truncate the input or any existing destination,
        // including aliases/symlinks. A failed export never replaces an old file.
        osl::File output(url);
        if (output.open(osl_File_OpenFlag_Write | osl_File_OpenFlag_Create)
            != osl::FileBase::E_None)
            throw std::runtime_error(
                "Choose a new filename; an existing file will not be overwritten");
        bool completed = false;
        comphelper::ScopeGuard removeIncomplete([&] {
            if (!completed)
            {
                output.close();
                osl::File::remove(url);
            }
        });
        bool ok = false;
        {
            SvFileStream input(temp.GetURL(), StreamMode::READ);
            unsigned char buffer[64 * 1024];
            ok = !input.GetError();
            while (ok)
            {
                const auto read = input.ReadBytes(buffer, sizeof(buffer));
                if (!read)
                    break;
                sal_uInt64 written = 0;
                ok = output.write(buffer, read, written) == osl::FileBase::E_None
                     && written == read;
            }
            ok = ok && !input.GetError() && output.sync() == osl::FileBase::E_None;
        }
        const auto closed = output.close();
        ok = ok && closed == osl::FileBase::E_None;
        if (!ok)
            throw std::runtime_error("Cannot write the exported PDF");
        completed = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = OUString::fromUtf8(exception.what());
    }
    catch (...)
    {
        error = u"Cannot export this PDF"_ustr;
    }
    return false;
}
}
}

extern "C" SAL_DLLPUBLIC_EXPORT pdffast::Editor*
pdffast_create_editor(vcl::Window* parent, const std::shared_ptr<pdffast::Source>& source,
                      const pdffast::EditorHost& host)
{
    return new pdffast::NativeEditor(parent, source, host);
}
