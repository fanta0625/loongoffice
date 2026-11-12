/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <sal/config.h>
#include "reader.hxx"
#include "cache.hxx"
#include "layout.hxx"
#include "session.hxx"
#include "transfer.hxx"
#include "editor.hxx"
#include "strings.hrc"
#include <comphelper/processfactory.hxx>
#include <com/sun/star/ui/dialogs/FilePicker.hpp>
#include <com/sun/star/ui/dialogs/XFilePicker3.hpp>
#include <com/sun/star/ui/dialogs/TemplateDescription.hpp>
#include <com/sun/star/ui/dialogs/ExecutableDialogResults.hpp>
#include <osl/module.hxx>
#include <tools/svlibrary.h>
#include <vcl/menu.hxx>
#include <vcl/toolkit/scrbar.hxx>
#include <vcl/dibtools.hxx>
#include <vcl/BitmapWriteAccess.hxx>
#include <vcl/outdev.hxx>
#include <vcl/region.hxx>
#include <vcl/wall.hxx>
#include <vcl/commandevent.hxx>
#include <vcl/event.hxx>
#include <vcl/keycodes.hxx>
#include <vcl/settings.hxx>
#include <vcl/svapp.hxx>
#include <vcl/timer.hxx>
#include <vcl/weld.hxx>
#include <sal/log.hxx>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <set>

namespace pdffast
{
namespace
{
constexpr int TileSize = 512;
constexpr tools::Long ScrollRange = 1000000;
enum Action : sal_uInt16
{
    Open = 100, Export, Close, Previous = 200, Next, GoTo, Fit, ZoomIn, ZoomOut,
    EditPage = 300, Read, Undo, Redo
};

class InputDialog final : public weld::GenericDialogController
{
    std::unique_ptr<weld::Label> m_label;
    std::unique_ptr<weld::Entry> m_entry;
public:
    InputDialog(weld::Window* parent, const OUString& prompt, bool password)
        : GenericDialogController(parent, u"pdffast/ui/input.ui"_ustr, u"InputDialog"_ustr)
        , m_label(m_xBuilder->weld_label(u"prompt"_ustr))
        , m_entry(m_xBuilder->weld_entry(u"value"_ustr))
    {
        m_label->set_label(prompt);
        m_entry->set_visibility(!password);
        m_entry->grab_focus();
    }
    OUString value() const { return m_entry->get_text(); }
};

class ReaderWindow;
class Canvas final : public vcl::Window
{
    ReaderWindow& m_owner;
public:
    Canvas(ReaderWindow& owner, vcl::Window* parent);
    void Paint(vcl::RenderContext& context, const tools::Rectangle& rect) override;
    void KeyInput(const KeyEvent& event) override;
    void Command(const CommandEvent& event) override;
    void MouseButtonDown(const MouseEvent& event) override { GrabFocus(); Window::MouseButtonDown(event); }
};

class ReaderWindow final : public WorkWindow
{
    VclPtr<Canvas> m_canvas;
    VclPtr<ScrollBar> m_vertical;
    VclPtr<ScrollBar> m_horizontal;
    VclPtr<MenuBar> m_menu;
    VclPtr<PopupMenu> m_fileMenu, m_viewMenu, m_editMenu;
    Timer m_poll{ "PDF Fast results" };
    Timer m_zoomDelay{ "PDF Fast zoom settling" };
    bool m_zoomSettling = false;
    double m_previousScroll = 0;
    int m_direction = 1;
    std::unique_ptr<Session> m_session;
    // Member order keeps the library loaded until all editor objects are gone.
    osl::Module m_editorLibrary;
    std::unique_ptr<Editor> m_editor;
    Cache<TileKey, Bitmap> m_cache;
    Layout m_layout;
    std::map<TileKey, std::size_t> m_visibleTiles;
    std::set<int> m_failedPages;
    std::set<int> m_editedPages;
    double m_scale = 1;
    double m_zoom = 1;
    double m_scrollX = 0;
    double m_scrollY = 0;
    bool m_ready = false;
    bool m_passwordProtected = false;
    bool m_reading = true;
    bool m_firstPaint = false;
    bool m_busy = false;
    int m_pendingEdit = -1;
    OUString m_url;
    OUString m_message;
    std::chrono::steady_clock::time_point m_started;

    DECL_LINK(Poll, Timer*, void);
    DECL_LINK(ZoomSettled, Timer*, void);
    DECL_LINK(Scrolled, ScrollBar*, void);
    DECL_LINK(Selected, Menu*, bool);
    void refresh();
    void scrollbars();
    void title();
    bool confirmClose();
    bool exportCopy();
    void editPage();
    void readMode();
    OUString pick(bool save);
    bool input(const OUString& prompt, bool password, OUString& value);
    void message(const OUString& value, VclMessageType type = VclMessageType::Error);
    double maxX() const { return std::max(0.0, m_layout.width() * m_scale - m_canvas->GetOutputSizePixel().Width()); }
    double scrollHeight() const
    {
        return m_layout.scrollHeight(m_canvas->GetOutputSizePixel().Height() / m_scale) * m_scale;
    }
    double maxY() const { return std::max(0.0, scrollHeight() - m_canvas->GetOutputSizePixel().Height()); }
    TileKey preview(int page) const;
    tools::Rectangle pageRect(int page) const;
    std::vector<int> visible() const;
    void zoom(double value);
    void go(int page);

public:
    ReaderWindow(std::size_t budget, std::chrono::steady_clock::time_point started);
    virtual ~ReaderWindow() override { disposeOnce(); }
    void dispose() override;
    bool Close() override;
    void Resize() override;
    void open(const OUString& url);
    void action(sal_uInt16 id);
    void paint(vcl::RenderContext& context);
    void key(const KeyEvent& event);
    void command(const CommandEvent& event);
};

Canvas::Canvas(ReaderWindow& owner, vcl::Window* parent)
    : Window(parent, WB_CLIPCHILDREN), m_owner(owner)
{
    SetBackground(Wallpaper(Color(0x70, 0x70, 0x70)));
}
void Canvas::Paint(vcl::RenderContext& context, const tools::Rectangle&) { m_owner.paint(context); }
void Canvas::KeyInput(const KeyEvent& event) { m_owner.key(event); }
void Canvas::Command(const CommandEvent& event) { m_owner.command(event); }

ReaderWindow::ReaderWindow(std::size_t budget, std::chrono::steady_clock::time_point started)
    : WorkWindow(nullptr, WB_APP | WB_STDWORK), m_cache(budget), m_started(started)
{
    m_canvas = VclPtr<Canvas>::Create(*this, this);
    m_vertical = VclPtr<ScrollBar>::Create(this, WB_VERT);
    m_horizontal = VclPtr<ScrollBar>::Create(this, WB_HORZ);
    m_vertical->EnableDrag();
    m_horizontal->EnableDrag();
    m_vertical->SetScrollHdl(LINK(this, ReaderWindow, Scrolled));
    m_horizontal->SetScrollHdl(LINK(this, ReaderWindow, Scrolled));
    m_menu = VclPtr<MenuBar>::Create();
    m_fileMenu = VclPtr<PopupMenu>::Create();
    m_viewMenu = VclPtr<PopupMenu>::Create();
    m_editMenu = VclPtr<PopupMenu>::Create();
    m_menu->InsertItem(1, tr(STR_FILE));
    m_menu->InsertItem(2, tr(STR_VIEW));
    m_menu->InsertItem(3, tr(STR_EDIT));
    m_menu->SetPopupMenu(1, m_fileMenu);
    m_menu->SetPopupMenu(2, m_viewMenu);
    m_menu->SetPopupMenu(3, m_editMenu);
    m_fileMenu->InsertItem(Open, tr(STR_OPEN));
    m_fileMenu->InsertItem(Export, tr(STR_EXPORT));
    m_fileMenu->InsertItem(Action::Close, tr(STR_CLOSE));
    m_viewMenu->InsertItem(Previous, tr(STR_PREVIOUS));
    m_viewMenu->InsertItem(Next, tr(STR_NEXT));
    m_viewMenu->InsertItem(GoTo, tr(STR_GOTO));
    m_viewMenu->InsertItem(Fit, tr(STR_FIT));
    m_viewMenu->InsertItem(ZoomIn, tr(STR_ZOOM_IN));
    m_viewMenu->InsertItem(ZoomOut, tr(STR_ZOOM_OUT));
    m_editMenu->InsertItem(EditPage, tr(STR_EDIT_PAGE));
    m_editMenu->InsertItem(Read, tr(STR_READ));
    m_editMenu->InsertItem(Undo, tr(STR_UNDO));
    m_editMenu->InsertItem(Redo, tr(STR_REDO));
    for (auto* menu : { m_fileMenu.get(), m_viewMenu.get(), m_editMenu.get() })
        menu->SetSelectHdl(LINK(this, ReaderWindow, Selected));
    SetMenuBar(m_menu);
    m_zoomDelay.SetTimeout(80);
    m_zoomDelay.SetInvokeHandler(LINK(this, ReaderWindow, ZoomSettled));
    m_poll.SetTimeout(8);
    m_poll.SetInvokeHandler(LINK(this, ReaderWindow, Poll));
    m_message = tr(STR_EMPTY);
    SetOutputSizePixel(Size(1100, 850));
    m_canvas->Show();
    m_vertical->Show();
    m_horizontal->Show();
    title();
}

void ReaderWindow::dispose()
{
    m_zoomDelay.Stop();
    m_poll.Stop();
    // Worker doesn't need SolarMutex or a UI callback to terminate.
    m_session.reset();
    m_editor.reset();
    m_cache.clear();
    m_visibleTiles.clear();
    SetMenuBar(nullptr);
    m_menu.disposeAndClear();
    m_fileMenu.disposeAndClear();
    m_viewMenu.disposeAndClear();
    m_editMenu.disposeAndClear();
    m_canvas.disposeAndClear();
    m_vertical.disposeAndClear();
    m_horizontal.disposeAndClear();
    WorkWindow::dispose();
}

bool ReaderWindow::Close()
{
    if (m_busy || !confirmClose())
        return false;
    m_poll.Stop();
    Hide();
    Application::Quit();
    return true;
}

void ReaderWindow::Resize()
{
    WorkWindow::Resize();
    if (!m_canvas || !m_vertical || !m_horizontal)
        return;
    const Size size = GetOutputSizePixel();
    const tools::Long bar = GetSettings().GetStyleSettings().GetScrollBarSize();
    const Size view(std::max<tools::Long>(1, size.Width() - bar),
                    std::max<tools::Long>(1, size.Height() - bar));
    m_canvas->SetPosSizePixel(Point(), view);
    m_vertical->SetPosSizePixel(Point(view.Width(), 0), Size(bar, view.Height()));
    m_horizontal->SetPosSizePixel(Point(0, view.Height()), Size(view.Width(), bar));
    if (m_ready)
        refresh();
}

void ReaderWindow::message(const OUString& value, VclMessageType type)
{
    auto dialog = std::unique_ptr<weld::MessageDialog>(Application::CreateMessageDialog(
        GetFrameWeld(), type, VclButtonsType::Ok, value));
    dialog->run();
}

bool ReaderWindow::input(const OUString& prompt, bool password, OUString& value)
{
    InputDialog dialog(GetFrameWeld(), prompt, password);
    if (dialog.run() != RET_OK)
        return false;
    value = dialog.value();
    return true;
}

OUString ReaderWindow::pick(bool save)
{
    try
    {
        auto picker = css::ui::dialogs::FilePicker::createWithMode(
            comphelper::getProcessComponentContext(), sal_Int16(save
            ? css::ui::dialogs::TemplateDescription::FILESAVE_AUTOEXTENSION
            : css::ui::dialogs::TemplateDescription::FILEOPEN_SIMPLE));
        picker->setTitle(tr(save ? STR_EXPORT : STR_OPEN));
        picker->appendFilter(u"PDF"_ustr, u"*.pdf"_ustr);
        if (save)
            picker->setDefaultName(u"edited.pdf"_ustr);
        if (picker->execute() == css::ui::dialogs::ExecutableDialogResults::OK)
        {
            auto files = picker->getSelectedFiles();
            if (files.hasElements())
                return files[0];
        }
    }
    catch (const css::uno::Exception& exception)
    {
        message(exception.Message);
    }
    return {};
}

bool ReaderWindow::confirmClose()
{
    if (!m_editor)
        return true;
    m_editor->finish();
    if (!m_editor->modified())
        return true;
    auto dialog = std::unique_ptr<weld::MessageDialog>(Application::CreateMessageDialog(
        GetFrameWeld(), VclMessageType::Question, VclButtonsType::NONE, tr(STR_SAVE_PROMPT)));
    dialog->add_button(tr(STR_EXPORT), RET_YES);
    dialog->add_button(tr(STR_DISCARD), RET_NO);
    dialog->add_button(tr(STR_CANCEL), RET_CANCEL);
    dialog->set_default_response(RET_CANCEL);
    const int response = dialog->run();
    return response == RET_NO || (response == RET_YES && exportCopy());
}

void ReaderWindow::open(const OUString& url)
{
    if (url.isEmpty() || m_busy || !confirmClose())
        return;
    if (!url.startsWith("file:"))
    {
        message(u"Only local PDF files are supported"_ustr);
        return;
    }
    m_zoomDelay.Stop();
    m_zoomSettling = false;
    m_poll.Stop();
    m_session.reset();
    m_editor.reset();
    m_cache.clear();
    m_visibleTiles.clear();
    m_failedPages.clear();
    m_editedPages.clear();
    m_ready = false;
    m_passwordProtected = false;
    m_reading = true;
    m_pendingEdit = -1;
    m_firstPaint = false;
    m_scrollX = m_scrollY = m_previousScroll = 0;
    m_direction = 1;
    m_url = url;
    m_message = tr(STR_LOADING);
    m_session = std::make_unique<Session>(url);
    title();
    m_canvas->Invalidate();
    m_poll.Start();
}

std::vector<int> ReaderWindow::visible() const
{
    std::vector<int> pages;
    if (!m_ready)
        return pages;
    const double bottom = (m_scrollY + m_canvas->GetOutputSizePixel().Height()) / m_scale;
    for (int page = m_layout.pageAt(m_scrollY / m_scale);
         page < m_layout.count() && m_layout.top(page) <= bottom && pages.size() < 4096; ++page)
        pages.push_back(page);
    return pages;
}

tools::Rectangle ReaderWindow::pageRect(int page) const
{
    const auto size = m_layout.size(page);
    const tools::Long width = std::ceil(size.width * m_scale);
    const tools::Long height = std::ceil(size.height * m_scale);
    const double content = std::max(m_layout.width() * m_scale,
                                   double(m_canvas->GetOutputSizePixel().Width()));
    return tools::Rectangle(Point(std::floor((content - width) / 2 - m_scrollX),
                                  std::floor(m_layout.top(page) * m_scale - m_scrollY)),
                            Size(width, height));
}

TileKey ReaderWindow::preview(int page) const
{
    const auto size = m_layout.size(page);
    const double scale = std::min(256 / size.width, 512 / size.height);
    const int width = std::max(1, int(std::ceil(size.width * scale)));
    const int height = std::max(1, int(std::ceil(size.height * scale)));
    return { page, width, height, 0, 0, width, height };
}

void ReaderWindow::scrollbars()
{
    m_scrollX = std::clamp(m_scrollX, 0.0, maxX());
    m_scrollY = std::clamp(m_scrollY, 0.0, maxY());
    const auto set = [](ScrollBar& bar, double viewport, double total, double position) {
        const tools::Long shown = std::clamp<tools::Long>(
            std::lround(ScrollRange * viewport / std::max(viewport, total)), 1, ScrollRange);
        bar.SetRangeMax(ScrollRange);
        bar.SetVisibleSize(shown);
        bar.SetPageSize(shown);
        bar.SetLineSize(std::max<tools::Long>(1, shown / 10));
        bar.SetThumbPos(total <= viewport ? 0 : std::lround(
            position / (total - viewport) * (ScrollRange - shown)));
    };
    const auto view = m_canvas->GetOutputSizePixel();
    set(*m_vertical, view.Height(), scrollHeight(), m_scrollY);
    set(*m_horizontal, view.Width(), m_layout.width() * m_scale, m_scrollX);
}

void ReaderWindow::refresh()
{
    if (!m_ready || !m_session)
        return;
    scrollbars();
    if (m_scrollY != m_previousScroll)
        m_direction = m_scrollY > m_previousScroll ? 1 : -1;
    m_previousScroll = m_scrollY;
    std::vector<Request> prefetch;
    std::map<TileKey, std::size_t> visiblePreviews;
    m_visibleTiles.clear();
    if (m_pendingEdit != m_layout.pageAt(m_scrollY / m_scale))
        m_pendingEdit = -1;
    std::vector<Request> requests;
    std::set<int> shown;
    const auto view = m_canvas->GetOutputSizePixel();
    for (int page : visible())
    {
        shown.insert(page);
        auto rect = pageRect(page);
        if (m_editor && m_editor->contains(page))
        {
            m_editor->place(page, rect, true);
            continue;
        }
        if (m_failedPages.count(page))
            continue;
        if (!m_layout.known(page))
        {
            requests.push_back({ page, {} });
            continue;
        }
        auto small = preview(page);
        const auto previewBytes = std::size_t(small.width) * small.height * 4;
        m_visibleTiles.emplace(small, previewBytes);
        visiblePreviews.emplace(small, previewBytes);
        if (!m_cache.contains(small))
            requests.push_back({ page, small });
        const int width = rect.GetWidth(), height = rect.GetHeight();
        if (m_zoomSettling || width > 1000000 || height > 1000000)
            continue;
        const int left = std::max<tools::Long>(0, -rect.Left());
        const int top = std::max<tools::Long>(0, -rect.Top());
        const int right = std::min<tools::Long>(width, view.Width() - rect.Left());
        const int bottom = std::min<tools::Long>(height, view.Height() - rect.Top());
        for (int y = top / TileSize * TileSize; y < bottom; y += TileSize)
            for (int x = left / TileSize * TileSize; x < right; x += TileSize)
            {
                TileKey key{ page, width, height, x, y,
                             std::min(TileSize, width - x), std::min(TileSize, height - y) };
                m_visibleTiles.emplace(key, std::size_t(key.width) * key.height * 4);
                if (!m_cache.contains(key))
                    requests.push_back({ page, key });
            }
        // One tile row just outside the viewport, at most eight speculative
        // tiles in total. Visible requests are always queued before this work.
        const int ahead = m_direction > 0 ? ((bottom + TileSize - 1) / TileSize) * TileSize
                                          : (top / TileSize - 1) * TileSize;
        if (ahead >= 0 && ahead < height)
            for (int x = left / TileSize * TileSize;
                 x < right && prefetch.size() < 8; x += TileSize)
            {
                TileKey key{ page, width, height, x, ahead,
                             std::min(TileSize, width - x), std::min(TileSize, height - ahead) };
                if (!m_cache.contains(key))
                    prefetch.push_back({ page, key });
            }
    }
    if (!m_zoomSettling && !shown.empty())
        for (int direction : { m_direction, -m_direction })
        {
            const int page = direction > 0 ? *shown.rbegin() + 1 : *shown.begin() - 1;
            if (page < 0 || page >= m_layout.count() || m_failedPages.count(page)
                || (m_editor && m_editor->contains(page)))
                continue;
            if (!m_layout.known(page))
                prefetch.push_back({ page, {} });
            else
            {
                if (auto small = preview(page); !m_cache.contains(small))
                    prefetch.push_back({ page, small });
                if (direction != m_direction)
                    continue;
                const auto rect = pageRect(page);
                const int width = rect.GetWidth(), height = rect.GetHeight();
                if (width > 1000000 || height > 1000000)
                    continue;
                const int y = direction > 0 ? 0 : (height - 1) / TileSize * TileSize;
                const int left = std::max<tools::Long>(0, -rect.Left());
                const int right = std::min<tools::Long>(width, view.Width() - rect.Left());
                for (int x = left / TileSize * TileSize;
                     x < right && prefetch.size() < 12; x += TileSize)
                {
                    TileKey key{ page, width, height, x, y,
                                 std::min(TileSize, width - x), std::min(TileSize, height - y) };
                    if (!m_cache.contains(key))
                        prefetch.push_back({ page, key });
                }
            }
        }
    m_cache.protect(m_visibleTiles, std::move(visiblePreviews));
    auto speculativeBytes = m_cache.spareForSpeculation();
    for (const auto& request : prefetch)
    {
        const auto bytes = std::size_t(request.tile.width) * request.tile.height * 4;
        if (bytes <= speculativeBytes)
        {
            auto background = request;
            background.prefetch = true;
            requests.push_back(background);
            speculativeBytes -= bytes;
        }
    }
    if (m_editor)
    {
        m_editor->readOnly(m_reading, m_zoomSettling);
        for (int page : m_editedPages)
            if (!shown.count(page))
                m_editor->place(page, {}, false);
    }
    m_session->request(std::move(requests));
    m_poll.Start();
    title();
    m_canvas->Invalidate();
}

void ReaderWindow::title()
{
    OUString text = u"LoongOffice PDF Fast"_ustr;
    if (!m_url.isEmpty())
        text = m_url.copy(m_url.lastIndexOf('/') + 1) + u" — "_ustr + text;
    if (m_ready)
        text += u" — "_ustr + tr(STR_PAGE_STATUS)
            .replaceFirst("%1", OUString::number(m_layout.pageAt(m_scrollY / m_scale) + 1))
            .replaceFirst("%2", OUString::number(m_layout.count()))
            .replaceFirst("%3", OUString::number(std::lround(m_zoom * 100)));
    if (m_editor && m_editor->modified())
        text += u" — "_ustr + tr(STR_MODIFIED);
    SetText(text);
    m_fileMenu->EnableItem(Export, m_ready && !m_passwordProtected && !m_busy);
    for (auto id : { Previous, Next, GoTo, Fit, ZoomIn, ZoomOut })
        m_viewMenu->EnableItem(id, m_ready && !m_busy);
    m_editMenu->EnableItem(EditPage, m_ready && !m_passwordProtected && !m_busy);
    m_editMenu->EnableItem(Read, bool(m_editor) && !m_busy);
    m_editMenu->EnableItem(Undo, bool(m_editor) && !m_busy);
    m_editMenu->EnableItem(Redo, bool(m_editor) && !m_busy);
}

IMPL_LINK_NOARG(ReaderWindow, Poll, Timer*, void)
{
    if (!m_session || m_busy)
        return;
    const auto transferStarted = std::chrono::steady_clock::now();
    std::size_t transferred = 0;
    bool layoutChanged = false;
    while (m_session && TransferBudget::allows(transferred,
               std::chrono::steady_clock::now() - transferStarted))
    {
        auto next = m_session->takeResult();
        if (!next)
            break;
        auto result = std::move(*next);
        ++transferred;
        switch (result.kind)
        {
            case Result::Kind::Opened:
                m_layout.reset(result.pageCount, result.size);
                m_ready = true;
                m_message.clear();
                m_scale = m_zoom * m_canvas->GetOutDev()->GetDPIX() / 72.0;
                layoutChanged = true;
                break;
            case Result::Kind::Page:
            {
                const int anchor = m_layout.pageAt(m_scrollY / m_scale);
                const double offset = m_scrollY - m_layout.top(anchor) * m_scale;
                m_layout.set(result.page, result.size);
                m_scrollY = m_layout.top(anchor) * m_scale + offset;
                layoutChanged = true;
                break;
            }
            case Result::Kind::Tile:
            {
                const auto imageStarted = std::chrono::steady_clock::now();
                Bitmap bitmap(Size(result.pixels.width, result.pixels.height), vcl::PixelFormat::N24_BPP);
                bool allocated = false;
                {
                    BitmapScopedWriteAccess access(bitmap);
                    allocated = bool(access);
                }
                const auto allocatedAt = std::chrono::steady_clock::now();
                if (allocated && ReadRawDIB(bitmap, result.pixels.data.data(), ScanlineFormat::N24BitTcBgr,
                               result.pixels.height, result.pixels.stride))
                    m_cache.put(result.tile, std::move(bitmap),
                                std::size_t(result.pixels.width) * result.pixels.height * 4,
                                !m_visibleTiles.count(result.tile));
                else
                {
                    m_message = u"Cannot allocate the page image"_ustr;
                    m_failedPages.insert(result.page);
                }
                SAL_INFO("pdffast", "ui-image page=" << result.page
                    << " width=" << result.pixels.width << " height=" << result.pixels.height
                    << " allocation_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
                        allocatedAt - imageStarted).count()
                    << " upload_cache_us=" << std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - allocatedAt).count());
                break;
            }
            case Result::Kind::Password:
            {
                m_passwordProtected = true;
                OUString password;
                if (input(tr(STR_PASSWORD), true, password))
                    m_session->unlock(OUStringToOString(password, RTL_TEXTENCODING_UTF8));
                else
                {
                    m_poll.Stop();
                    m_session.reset();
                    m_message = tr(STR_EMPTY);
                }
                break;
            }
            case Result::Kind::Error:
                m_message = result.message;
                if (result.requested)
                    m_failedPages.insert(result.page);
                else
                    m_poll.Stop();
                break;
        }
    }
    if (layoutChanged)
        refresh();
    if (m_pendingEdit >= 0 && m_layout.known(m_pendingEdit))
    {
        m_pendingEdit = -1;
        editPage();
    }
    if (transferred)
    {
        m_canvas->Invalidate();
        SAL_INFO("pdffast", "UI result batch " << transferred << " items in "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - transferStarted).count() << " us");
    }
    if (m_session && !m_session->idle())
    {
        m_poll.SetTimeout(m_session->hasResults() ? 1 : 8);
        m_poll.Start();
    }
}

IMPL_LINK(ReaderWindow, Scrolled, ScrollBar*, bar, void)
{
    if (!m_ready)
        return;
    const double range = bar->GetRangeMax() - bar->GetVisibleSize();
    const double fraction = range > 0 ? bar->GetThumbPos() / range : 0;
    if (bar == m_vertical)
        m_scrollY = fraction * maxY();
    else
        m_scrollX = fraction * maxX();
    refresh();
}

IMPL_LINK(ReaderWindow, Selected, Menu*, menu, bool)
{
    action(menu->GetCurItemId());
    return true;
}

void ReaderWindow::go(int page)
{
    if (!m_ready)
        return;
    page = std::clamp(page, 0, m_layout.count() - 1);
    // Stay on the destination side of a page boundary after pixel/point
    // floating-point round trips, including fractional DPI and zoom factors.
    m_scrollY = std::nextafter(m_layout.top(page) * m_scale,
                             std::numeric_limits<double>::infinity());
    refresh();
}

IMPL_LINK_NOARG(ReaderWindow, ZoomSettled, Timer*, void)
{
    m_zoomSettling = false;
    refresh();
}

void ReaderWindow::zoom(double value)
{
    if (!m_ready)
        return;
    m_zoomSettling = true;
    m_zoomDelay.Start();
    const double old = m_scale;
    m_zoom = std::clamp(value, 0.125, 8.0);
    m_scale = m_zoom * m_canvas->GetOutDev()->GetDPIX() / 72.0;
    m_scrollX *= m_scale / old;
    m_scrollY *= m_scale / old;
    refresh();
}

void ReaderWindow::readMode()
{
    // Prepared pages remain visible and retain their models; reading mode only
    // disables their input, never replaces edited content with the source PDF.
    m_reading = true;
    if (m_editor)
        m_editor->finish();
    refresh();
    m_canvas->GrabFocus();
}

void ReaderWindow::editPage()
{
    if (!m_ready || m_busy)
        return;
    if (m_passwordProtected)
    {
        message(tr(STR_PROTECTED_EDIT));
        return;
    }
    const int page = m_layout.pageAt(m_scrollY / m_scale);
    if (m_failedPages.count(page))
        return;
    if (!m_layout.known(page))
    {
        m_pendingEdit = page;
        refresh();
        return;
    }
    const auto editStarted = std::chrono::steady_clock::now();
    m_busy = true;
    title();
    OUString error;
    try
    {
        if (!m_editor)
        {
            if (!m_editorLibrary.is()
                && !m_editorLibrary.loadRelative(reinterpret_cast<oslGenericFunction>(&InitVCL),
                                                  SVLIBRARY("pdffastedit")))
                error = tr(STR_EDITOR_MISSING);
            else
            {
                auto create = reinterpret_cast<CreateEditor>(
                    m_editorLibrary.getFunctionSymbol("pdffast_create_editor"));
                if (create)
                {
                    EditorHost host;
                    host.changed = [this] { title(); };
                    host.command = [this](const CommandEvent& event) { command(event); };
                    host.key = [this](const KeyEvent& event) { key(event); };
                    m_editor.reset(create(m_canvas, m_session->source(), host));
                }
                if (!m_editor)
                    error = tr(STR_EDITOR_MISSING);
            }
        }
        if (m_editor && m_editor->prepare(page, m_layout.size(page), error))
        {
            m_editedPages.insert(page);
            m_reading = false;
        }
        SAL_INFO("pdffast", "prepare edit page " << page << " in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - editStarted).count() << " ms");
    }
    catch (const std::exception& exception)
    {
        error = OUString::fromUtf8(exception.what());
    }
    catch (...)
    {
        error = tr(STR_EDITOR_MISSING);
    }
    m_busy = false;
    refresh();
    if (!error.isEmpty())
        message(error);
    else if (m_editor)
        m_editor->focus(page);
}

bool ReaderWindow::exportCopy()
{
    if (!m_ready || m_busy || m_passwordProtected)
        return false;
    auto dialog = std::unique_ptr<weld::MessageDialog>(Application::CreateMessageDialog(
        GetFrameWeld(), VclMessageType::Warning, VclButtonsType::OkCancel, tr(STR_EXPORT_NOTICE)));
    if (dialog->run() != RET_OK)
        return false;
    auto url = pick(true);
    if (url.isEmpty())
        return false;
    // Loading the editor on export is allowed, but never on the reading first screen.
    // No page conversion is needed just to create the export backend.
    try
    {
        if (!m_editor)
        {
            // The same factory path as first edit, without implicitly converting a page.
            if (!m_editorLibrary.is()
                && !m_editorLibrary.loadRelative(reinterpret_cast<oslGenericFunction>(&InitVCL),
                                                  SVLIBRARY("pdffastedit")))
            {
                message(tr(STR_EDITOR_MISSING));
                return false;
            }
            auto create = reinterpret_cast<CreateEditor>(m_editorLibrary.getFunctionSymbol("pdffast_create_editor"));
            if (!create)
                return false;
            EditorHost host{ [this] { title(); }, [this](const CommandEvent& e) { command(e); },
                             [this](const KeyEvent& e) { key(e); } };
            m_editor.reset(create(m_canvas, m_session->source(), host));
        }
        if (!m_editor)
            return false;
    }
    catch (const std::exception& exception)
    {
        message(OUString::fromUtf8(exception.what()));
        return false;
    }
    catch (...)
    {
        message(tr(STR_EDITOR_MISSING));
        return false;
    }
    m_busy = true;
    title();
    OUString error;
    const bool ok = m_editor->exportCopy(url, error);
    m_busy = false;
    title();
    if (!ok)
        message(error);
    return ok;
}

void ReaderWindow::action(sal_uInt16 id)
{
    if (m_busy)
        return;
    switch (id)
    {
        case Open: open(pick(false)); break;
        case Export: exportCopy(); break;
        case Action::Close: Close(); break;
        case Previous: if (m_ready) go(m_layout.pageAt(m_scrollY / m_scale) - 1); break;
        case Next: if (m_ready) go(m_layout.pageAt(m_scrollY / m_scale) + 1); break;
        case GoTo:
        {
            OUString value;
            if (m_ready && input(tr(STR_PAGE_NUMBER), false, value))
            {
                const auto page = value.toInt32();
                if (page > 0 && page <= m_layout.count())
                    go(page - 1);
            }
            break;
        }
        case Fit:
            if (m_ready)
                zoom((m_canvas->GetOutputSizePixel().Width() - 2 * Layout::Gap)
                     / m_layout.size(m_layout.pageAt(m_scrollY / m_scale)).width
                     * 72.0 / m_canvas->GetOutDev()->GetDPIX());
            break;
        case ZoomIn: zoom(m_zoom * 1.25); break;
        case ZoomOut: zoom(m_zoom / 1.25); break;
        case EditPage: editPage(); break;
        case Read: readMode(); break;
        case Undo: if (m_editor) m_editor->undo(false); break;
        case Redo: if (m_editor) m_editor->undo(true); break;
    }
}

void ReaderWindow::key(const KeyEvent& event)
{
    const auto& key = event.GetKeyCode();
    if (key.IsMod1())
    {
        switch (key.GetCode())
        {
            case KEY_O: action(Open); return;
            case KEY_S: action(Export); return;
            case KEY_G: action(GoTo); return;
            case KEY_ADD: action(ZoomIn); return;
            case KEY_SUBTRACT: action(ZoomOut); return;
            case KEY_Z: action(key.IsShift() ? Redo : Undo); return;
            case KEY_Y: action(Redo); return;
        }
    }
    if (!m_ready || m_busy)
        return;
    switch (key.GetCode())
    {
        case KEY_F2: action(EditPage); return;
        case KEY_ESCAPE: action(Read); return;
        case KEY_HOME: go(0); return;
        case KEY_END: go(m_layout.count() - 1); return;
        case KEY_PAGEUP: m_scrollY -= m_canvas->GetOutputSizePixel().Height() * 0.9; break;
        case KEY_PAGEDOWN: m_scrollY += m_canvas->GetOutputSizePixel().Height() * 0.9; break;
        case KEY_UP: m_scrollY -= 40; break;
        case KEY_DOWN: m_scrollY += 40; break;
        case KEY_LEFT: m_scrollX -= 40; break;
        case KEY_RIGHT: m_scrollX += 40; break;
        default: return;
    }
    refresh();
}

void ReaderWindow::command(const CommandEvent& event)
{
    if (!m_ready || m_busy || event.GetCommand() != CommandEventId::Wheel || !event.GetWheelData())
        return;
    const auto& wheel = *event.GetWheelData();
    if (wheel.IsMod1())
        zoom(wheel.GetDelta() > 0 ? m_zoom * 1.1 : m_zoom / 1.1);
    else
    {
        const double delta = wheel.IsDeltaPixel() ? -wheel.GetDelta() : -wheel.GetDelta() / 120.0 * 120;
        if (wheel.IsHorz() || wheel.IsShift())
            m_scrollX += delta;
        else
            m_scrollY += delta;
        refresh();
    }
}

void ReaderWindow::paint(vcl::RenderContext& context)
{
    context.Push(vcl::PushFlags::ALL);
    context.SetMapMode(MapMode(MapUnit::MapPixel));
    context.SetLineColor();
    context.SetFillColor(Color(0x70, 0x70, 0x70));
    context.DrawRect(tools::Rectangle(Point(), m_canvas->GetOutputSizePixel()));
    context.SetTextColor(COL_WHITE);
    if (!m_ready)
        context.DrawText(Point(24, 32), m_message);
    else
    {
        const auto view = m_canvas->GetOutputSizePixel();
        for (int page : visible())
        {
            auto rect = pageRect(page);
            context.SetFillColor(COL_WHITE);
            context.DrawRect(rect);
            if (m_editor && m_editor->contains(page))
                continue; // Child draws the edited model, including in reading mode.
            if (m_failedPages.count(page))
            {
                context.SetTextColor(COL_BLACK);
                context.DrawText(rect.TopLeft() + Point(12, 24), m_message);
                continue;
            }
            bool drew = false;
            const int width = rect.GetWidth(), height = rect.GetHeight();
            const int left = std::max<tools::Long>(0, -rect.Left());
            const int top = std::max<tools::Long>(0, -rect.Top());
            const int right = std::min<tools::Long>(width, view.Width() - rect.Left());
            const int bottom = std::min<tools::Long>(height, view.Height() - rect.Top());
            std::vector<TileKey> current, missing;
            for (int y = top / TileSize * TileSize; y < bottom; y += TileSize)
                for (int x = left / TileSize * TileSize; x < right; x += TileSize)
                {
                    TileKey key{ page, width, height, x, y,
                                 std::min(TileSize, width - x), std::min(TileSize, height - y) };
                    current.push_back(key);
                    if (!m_cache.contains(key))
                    {
                        TileKey gap = key;
                        gap.x = std::max(x, left);
                        gap.y = std::max(y, top);
                        gap.width = std::min(x + key.width, right) - gap.x;
                        gap.height = std::min(y + key.height, bottom) - gap.y;
                        missing.push_back(gap);
                    }
                }
            // Once exact coverage is complete, do not draw or touch preview/old
            // images. While incomplete, clip every fallback to the actual holes.
            if (!missing.empty())
            {
                context.Push(vcl::PushFlags::CLIPREGION);
                vcl::Region holes;
                for (const auto& gap : missing)
                    holes.Union(tools::Rectangle(rect.TopLeft() + Point(gap.x, gap.y),
                                                 Size(gap.width, gap.height)));
                context.IntersectClipRegion(holes);
                if (const auto* small = m_cache.get(preview(page)))
                {
                    context.DrawBitmap(rect.TopLeft(), rect.GetSize(), *small);
                    drew = true;
                }
                const auto oldKeys = m_cache.keys([&](const TileKey& key) {
                    return key.page == page && key.pageWidth != width && key.pageWidth > 256
                        && std::any_of(missing.begin(), missing.end(), [&](const TileKey& gap) {
                            return fallbackIntersects(key, gap);
                        });
                });
                int fallbackWidth = 0, fallbackHeight = 0;
                double distance = std::numeric_limits<double>::infinity();
                for (const auto& key : oldKeys)
                {
                    const double candidate = std::abs(std::log(double(key.pageWidth) / width));
                    if (candidate < distance)
                    {
                        distance = candidate;
                        fallbackWidth = key.pageWidth;
                        fallbackHeight = key.pageHeight;
                    }
                }
                for (const auto& key : oldKeys)
                {
                    if (key.pageWidth != fallbackWidth || key.pageHeight != fallbackHeight)
                        continue;
                    const Point at(std::floor(double(key.x) * width / key.pageWidth),
                                   std::floor(double(key.y) * height / key.pageHeight));
                    const Size size(std::ceil(double(key.x + key.width) * width / key.pageWidth) - at.X(),
                                    std::ceil(double(key.y + key.height) * height / key.pageHeight) - at.Y());
                    if (const auto* bitmap = m_cache.get(key))
                    {
                        context.DrawBitmap(rect.TopLeft() + at, size, *bitmap);
                        drew = true;
                    }
                }
                context.Pop();
            }
            for (const auto& key : current)
                if (const auto* bitmap = m_cache.get(key))
                {
                    context.DrawBitmap(rect.TopLeft() + Point(key.x, key.y), *bitmap);
                    drew = true;
                }
            if (drew && !m_firstPaint)
            {
                m_firstPaint = true;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - m_started).count();
                SAL_INFO("pdffast", "first PDF pixels painted after " << ms << " ms");
            }
        }
    }
    context.Pop();
}
}

VclPtr<WorkWindow> createReader(const OUString& url, std::size_t budget,
                               std::chrono::steady_clock::time_point started)
{
    auto reader = VclPtr<ReaderWindow>::Create(budget, started);
    if (!url.isEmpty())
        reader->open(url);
    return reader;
}
}
