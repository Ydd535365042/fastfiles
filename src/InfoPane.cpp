// InfoPane window: header tabs, Details page painting, Preview page children (text edit, archive list, handler host).
#include "InfoPane.h"
#include "Format.h"
#include "Lang.h"
#include "DarkMode.h"
#include "MediaPreview.h"
#include "DWriteText.h"
#include <uxtheme.h>
#include <richedit.h>
#include <richole.h>
#include <tom.h>

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.InfoPane";
constexpr int kHeader = 44, kPad = 14, kPreviewHead = 52;

std::wstring FormatBytes(ULONGLONG bytes)
{
    return Format::Size(bytes); // Files' ToSizeString
}

bool FontExists(const wchar_t* face)
{
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    HDC hdc = GetDC(nullptr);
    EnumFontFamiliesExW(hdc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) { *reinterpret_cast<bool*>(p) = true; return 0; },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, hdc);
    return found;
}

// Scale down (never up) and center; alpha-blend when the bitmap carries alpha
void DrawFit(HDC hdc, HBITMAP bmp, bool alpha, const RECT& box)
{
    BITMAP bm{};
    if (!bmp || !GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0)
        return;
    const int srcW = bm.bmWidth, srcH = std::abs(bm.bmHeight);
    const int boxW = box.right - box.left, boxH = box.bottom - box.top;
    if (boxW <= 0 || boxH <= 0)
        return;
    const double scale = std::min({ 1.0, boxW / static_cast<double>(srcW), boxH / static_cast<double>(srcH) });
    const int w = std::max(1, static_cast<int>(srcW * scale)), h = std::max(1, static_cast<int>(srcH * scale));
    const int x = box.left + (boxW - w) / 2, y = box.top + (boxH - h) / 2;
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (alpha)
    {
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, x, y, w, h, mem, 0, 0, srcW, srcH, bf);
    }
    else
    {
        const int oldMode = SetStretchBltMode(hdc, HALFTONE);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchBlt(hdc, x, y, w, h, mem, 0, 0, srcW, srcH, SRCCOPY);
        SetStretchBltMode(hdc, oldMode);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
}

// The previews' clipping parent: its children are wider / taller than it (their system scroll bars fall outside);
// their colors and notifications go on to the pane
constexpr wchar_t kClipClass[] = L"FastFiles.InfoPaneClip";
LRESULT CALLBACK ClipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1; // the visible child covers it
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_NOTIFY: case WM_COMMAND:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

InfoPane::~InfoPane()
{
    Destroy();
}

bool InfoPane::Create(HWND parent, int id, const Theme* theme)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    m_theme = theme;
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered || !CreateWindowExW(0, kClass, Tr(L"详情窗格"), WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
            parent, MenuId(id), inst, this))
        return false;
    m_bar.Attach(m_hwnd, true, kBarTimer, m_theme, [this](int y) { m_smooth.Reset(y); ScrollTo(y); });
    static const bool clipRegistered = [inst]
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ClipProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClipClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    m_clip = clipRegistered ? CreateWindowExW(WS_EX_CONTROLPARENT, kClipClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, m_hwnd, nullptr, inst, nullptr)
                            : nullptr;
    const HWND previews = m_clip ? m_clip : m_hwnd;
    m_text = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
        ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL, 0, 0, 0, 0, previews, nullptr, inst, nullptr);
    SendMessageW(m_text, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    m_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SHAREIMAGELISTS |
        LVS_NOSORTHEADER | LVS_SINGLESEL, 0, 0, 0, 0, previews, nullptr, inst, nullptr);
    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(m_list, L"Explorer", nullptr);
    SHFILEINFOW sfi{};
    m_images = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L".txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    ListView_SetImageList(m_list, m_images, LVSIL_SMALL);
    LVCOLUMNW col{ LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
    col.pszText = const_cast<wchar_t*>(Tr(L"名称"));
    col.cx = 200;
    ListView_InsertColumn(m_list, 0, &col);
    col.pszText = const_cast<wchar_t*>(Tr(L"大小"));
    col.fmt = LVCFMT_RIGHT;
    col.cx = 80;
    ListView_InsertColumn(m_list, 1, &col);
    m_host = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_CLIPCHILDREN | SS_NOTIFY, 0, 0, 0, 0, m_hwnd, nullptr, inst, nullptr);
    // Files' RichEditBox / RichTextBlock previews: a read-only RichEdit (Msftedit) fed RTF
    static const bool richLoaded = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    if (richLoaded)
    {
        // ES_DISABLENOSCROLL: both bars always there (cut off by m_clip), so the text's width doesn't jump with them
        m_rich = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL | ES_DISABLENOSCROLL, 0, 0, 0, 0, previews, nullptr, inst, nullptr);
        SendMessageW(m_rich, EM_SETEVENTMASK, 0, ENM_LINK);
        SendMessageW(m_rich, EM_SETEDITSTYLEEX, SES_EX_HANDLEFRIENDLYURL, SES_EX_HANDLEFRIENDLYURL);
        SendMessageW(m_rich, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    }
    if (m_clip)
    {
        // (ids 0xFF51.. : the controls' own timers are small numbers)
        m_textBars.Attach(m_text, NativeBars::Kind::Edit, m_theme, 0xFF51, 0xFF52);
        m_listBars.Attach(m_list, NativeBars::Kind::List, m_theme, 0xFF51, 0xFF52);
        m_richBars.Attach(m_rich, NativeBars::Kind::Rich, m_theme, 0xFF51, 0xFF52);
    }
    m_media = new MediaPreview();
    if (!m_media->Create(m_hwnd, m_theme))
    {
        delete m_media;
        m_media = nullptr;
    }
    m_contentBrush = CreateSolidBrush(m_theme->content);
    Relayout();
    return true;
}

void InfoPane::Destroy()
{
    m_smooth.Reset(m_scroll); // drops it from the frame clock before the window goes
    UnloadHandler();
    delete m_media; // stops playback before the window goes
    m_media = nullptr;
    if (m_hwnd)
        DestroyWindow(m_hwnd);
    m_hwnd = m_text = m_list = m_host = m_rich = m_clip = nullptr;
    for (HGDIOBJ* o : { reinterpret_cast<HGDIOBJ*>(&m_mono), reinterpret_cast<HGDIOBJ*>(&m_bold), reinterpret_cast<HGDIOBJ*>(&m_contentBrush) })
        if (*o)
        {
            DeleteObject(*o);
            *o = nullptr;
        }
    ClearThumb();
}

void InfoPane::ClearThumb()
{
    if (m_thumb)
        DeleteObject(m_thumb);
    m_thumb = nullptr;
}

void InfoPane::Move(const RECT& rc)
{
    if (m_hwnd)
        MoveChild(m_hwnd, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

void InfoPane::Relayout()
{
    const Theme& t = *m_theme;
    if (m_mono)
        DeleteObject(m_mono);
    static const bool cascadia = FontExists(L"Cascadia Mono");
    m_mono = CreateFontW(-t.S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, cascadia ? L"Cascadia Mono" : L"Consolas");
    if (m_bold)
        DeleteObject(m_bold);
    LOGFONTW lf{};
    GetObjectW(t.ui, sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    m_bold = CreateFontIndirectW(&lf);
    SendMessageW(m_text, WM_SETFONT, reinterpret_cast<WPARAM>(m_mono), FALSE);
    SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(t.ui), FALSE);
    ListView_SetBkColor(m_list, t.content);
    ListView_SetTextBkColor(m_list, t.content);
    ListView_SetTextColor(m_list, t.ink);
    // Palette may have changed (light/dark): edit background brush and the Windows-drawn scroll bars / header
    if (m_contentBrush)
        DeleteObject(m_contentBrush);
    m_contentBrush = CreateSolidBrush(t.content);
    if (m_darkApplied != t.dark)
    {
        m_darkApplied = t.dark;
        DarkMode::ApplyControl(m_list, t.dark);
        DarkMode::ApplyControl(ListView_GetHeader(m_list), t.dark, L"ItemsView", L"DarkMode_ItemsView");
        DarkMode::ApplyControl(m_text, t.dark);
        if (m_rich)
            DarkMode::ApplyControl(m_rich, t.dark);
    }
    DarkMode::ColorHeaderText(m_list, &t.muted);
    InvalidateRect(m_text, nullptr, TRUE);
    if (m_rich)
    {
        SendMessageW(m_rich, EM_SETBKGNDCOLOR, 0, t.content);
        if (!m_rtf.empty() && m_kind == Kind::Rich && m_pidl)
            m_rtfFile ? SetRich() : Start(); // generated RTF carries the theme's colors: build it again
    }
    if (m_media)
        InvalidateRect(m_media->Hwnd(), nullptr, FALSE);
    LayoutChildren();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Geometry ---

RECT InfoPane::BodyRect() const
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const Theme& t = *m_theme;
    return { t.S(kPad), t.S(kHeader), std::max<LONG>(t.S(kPad), rc.right - t.S(kPad)), std::max<LONG>(t.S(kHeader), rc.bottom - t.S(10)) };
}

// Files' InfoPane "Horizontal" state (pane under the file area): preview on the left, properties beside it
bool InfoPane::Horizontal() const
{
    const RECT body = BodyRect();
    const int w = body.right - body.left, h = body.bottom - body.top;
    return w > m_theme->S(420) && w > h * 2;
}

RECT InfoPane::ThumbRect() const
{
    const RECT body = BodyRect();
    const int w = body.right - body.left;
    if (Horizontal())
    {
        // Fixed square to the left, the height of the pane (the details scroll beside it)
        const int side = std::clamp<int>(body.bottom - body.top, m_theme->S(96), m_theme->S(220));
        return { body.left, body.top, body.left + side, body.top + side };
    }
    return { body.left, body.top - m_scroll, body.right, body.top - m_scroll + std::clamp(w * 3 / 4, m_theme->S(96), m_theme->S(220)) };
}

// Where the name, type and property grid go: under the preview, or beside it
RECT InfoPane::DetailsRect() const
{
    const RECT body = BodyRect();
    if (Horizontal())
        return { ThumbRect().right + m_theme->S(16), body.top - m_scroll, body.right, body.bottom };
    return { body.left, ThumbRect().bottom + m_theme->S(12), body.right, body.bottom };
}

void InfoPane::ScrollTo(int offset)
{
    const RECT body = BodyRect();
    offset = std::clamp(offset, 0, std::max(0, m_contentHeight - static_cast<int>(body.bottom - body.top)));
    if (offset != m_scroll)
    {
        m_scroll = offset;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    UpdateBar();
}

// The Details page's ScrollViewer bar along the pane's right edge, over the body (the Preview page's controls scroll themselves)
void InfoPane::UpdateBar()
{
    if (!m_hwnd || !m_theme)
        return;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const RECT body = BodyRect();
    const int view = body.bottom - body.top;
    const RECT strip{ client.right - m_theme->S(12), body.top, client.right, body.bottom };
    m_bar.Set(m_page == Page::Details && m_contentHeight > view ? m_contentHeight : 0, view, m_scroll, strip, m_theme->S(32));
}

int InfoPane::HitTab(POINT pt) const
{
    const Theme& t = *m_theme;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int top = t.S(8), bottom = t.S(kHeader - 8);
    if (pt.y < top || pt.y >= bottom)
        return -1;
    if (pt.x >= t.S(kPad) && pt.x < t.S(kPad + 56))
        return 0;
    if (pt.x >= t.S(kPad + 60) && pt.x < t.S(kPad + 116))
        return 1;
    if (pt.x >= rc.right - t.S(kPad + 28) && pt.x < rc.right - t.S(kPad - 4))
        return 2;
    return -1;
}

void InfoPane::LayoutChildren()
{
    if (!m_hwnd)
        return;
    const Theme& t = *m_theme;
    RECT body = BodyRect();
    body.top += t.S(kPreviewHead);
    if (m_kind == Kind::Archive)
        body.top += t.S(44); // the two summary rows
    const int w = std::max<int>(0, body.right - body.left), h = std::max<int>(0, body.bottom - body.top);
    if (m_clip)
    {
        // The clip shows exactly the body; each control is as much bigger as its system bars take, so its client area
        // is the body and its bars lie outside (a bar the control hides or shows changes that: placed again)
        MoveChild(m_clip, body.left, body.top, w, h);
        const int cx = GetSystemMetricsForDpi(SM_CXVSCROLL, t.dpi), cy = GetSystemMetricsForDpi(SM_CYHSCROLL, t.dpi);
        for (HWND c : { m_text, m_list, m_rich })
            for (int pass = 0; c && pass < 3; ++pass)
            {
                const LONG_PTR style = GetWindowLongPtrW(c, GWL_STYLE);
                const int ew = w + ((style & WS_VSCROLL) ? cx : 0), eh = h + ((style & WS_HSCROLL) ? cy : 0);
                RECT now{};
                GetWindowRect(c, &now);
                if (now.right - now.left == ew && now.bottom - now.top == eh && pass > 0)
                    break;
                MoveChild(c, 0, 0, ew, eh);
            }
        ShowWindow(m_clip, (GetWindowLongPtrW(m_text, GWL_STYLE) & WS_VISIBLE) || (GetWindowLongPtrW(m_list, GWL_STYLE) & WS_VISIBLE) ||
            (m_rich && (GetWindowLongPtrW(m_rich, GWL_STYLE) & WS_VISIBLE)) ? SW_SHOWNA : SW_HIDE);
    }
    else
    {
        MoveChild(m_text, body.left, body.top, w, h);
        MoveChild(m_list, body.left, body.top, w, h);
        if (m_rich)
            MoveChild(m_rich, body.left, body.top, w, h);
    }
    ListView_SetColumnWidth(m_list, 1, t.S(80));
    // (room for the overlay bar on the right: WinUI's sits over the list, beside the sizes)
    ListView_SetColumnWidth(m_list, 0, std::max(t.S(80), w - t.S(80) - (m_clip ? t.S(12) : GetSystemMetricsForDpi(SM_CXVSCROLL, t.dpi)) - t.S(4)));
    MoveChild(m_host, body.left, body.top, w, h);
    if (m_media)
        m_media->Move({ body.left, body.top, body.left + w, body.top + h });
    if (m_handler)
    {
        RECT hr{ 0, 0, w, h };
        m_handler->SetRect(&hr);
    }
}

// --- Content ---

void InfoPane::SetPage(Page page)
{
    if (page == m_page)
        return;
    m_page = page;
    m_scroll = 0;
    m_smooth.Reset(0);
    UnloadHandler();
    ApplyPreview();
    if (m_kind != Kind::Many)
        Start();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

std::vector<int> InfoPane::FocusParts() const
{
    std::vector<int> parts;
    if (!m_hwnd || !IsWindowVisible(m_hwnd))
        return parts;
    parts = { PartPages, PartClose };
    if (m_page == Page::Details && m_tagsShown && m_kind != Kind::Many)
        parts.push_back(PartTags);
    return parts;
}

void InfoPane::FocusPart(int part)
{
    m_focusPart = part;
    m_ring = true;
    m_revealTags = part == PartTags;
    NameFocus();
    SetFocus(m_hwnd);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// The window's name is the focused button's (what a screen reader reads, as the toolbar's; tests read it too)
void InfoPane::NameFocus()
{
    // (the labels it shows: no new strings)
    const wchar_t* name = m_focusPart == PartClose ? Tr(L"关闭") : m_focusPart == PartTags ? (m_tags.empty() ? Tr(L"+ 添加标签") : Tr(L"编辑"))
                        : m_page == Page::Details ? Tr(L"详情") : Tr(L"预览");
    SetWindowTextW(m_hwnd, name);
}

int InfoPane::FocusedPart() const
{
    return GetFocus() == m_hwnd ? m_focusPart : -1;
}

void InfoPane::Press(int part)
{
    switch (part)
    {
    case PartPages:
        break; // (a RadioButton: Space on the checked one does nothing)
    case PartClose:
        m_events->OnInfoPaneClose();
        break;
    case PartTags:
        if (!IsRectEmpty(&m_tagsButton))
        {
            POINT screen{ m_tagsButton.left, m_tagsButton.bottom };
            ClientToScreen(m_hwnd, &screen);
            m_events->OnInfoPaneTags(screen);
        }
        break;
    }
}

void InfoPane::ShowItem(PCIDLIST_ABSOLUTE pidl)
{
    if (!pidl || (m_pidl && m_kind != Kind::Many && ILIsEqual(m_pidl.get(), pidl)))
        return;
    m_pidl = ClonePidl(pidl);
    m_scroll = 0;
    m_smooth.Reset(0);
    m_extra.clear();
    UnloadHandler();
    if (m_kind == Kind::Many)
    {
        m_kind = Kind::None;
        m_props.clear();
    }
    Start();
}

void InfoPane::ShowMany(int count, const std::wstring& size)
{
    ++m_generation; // drop any result still in flight
    UnloadHandler();
    ClearThumb();
    m_pidl.reset();
    m_kind = Kind::Many;
    m_name = Tr(L"已选择 ") + std::to_wstring(count) + Tr(L" 个项目");
    m_type = size.empty() ? L"" : Tr(L"共 ") + size;
    m_note.clear();
    m_props.clear();
    m_extra.clear();
    m_archive.clear();
    m_scroll = 0;
    m_smooth.Reset(0);
    ApplyPreview();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void InfoPane::SetExtra(PCIDLIST_ABSOLUTE pidl, std::vector<std::pair<std::wstring, std::wstring>> rows)
{
    if (!m_pidl || !pidl || !ILIsEqual(m_pidl.get(), pidl) || rows == m_extra)
        return;
    m_extra = std::move(rows);
    if (m_page == Page::Details)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void InfoPane::SetTags(std::vector<std::pair<std::wstring, COLORREF>> tags, bool shown)
{
    if (tags == m_tags && shown == m_tagsShown)
        return;
    m_tags = std::move(tags);
    m_tagsShown = shown;
    if (m_page == Page::Details)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void InfoPane::ApplyPreview()
{
    const bool preview = m_page == Page::Preview;
    ShowWindow(m_text, preview && m_kind == Kind::Text ? SW_SHOWNA : SW_HIDE);
    ListView_SetItemCountEx(m_list, static_cast<int>(m_archive.size()), LVSICF_NOSCROLL);
    ShowWindow(m_list, preview && m_kind == Kind::Archive ? SW_SHOWNA : SW_HIDE);
    if (m_rich)
        ShowWindow(m_rich, preview && m_kind == Kind::Rich ? SW_SHOWNA : SW_HIDE);
    if (m_media)
    {
        m_media->Show(preview && m_kind == Kind::Media);
        // Leaving the preview (or the file) stops the player, as Files unloads its MediaPlayerElement
        if (!preview || m_kind != Kind::Media)
        {
            m_media->Unload();
            m_mediaPath.clear();
        }
    }
    LayoutChildren();
    if (!preview || m_kind != Kind::Handler)
    {
        UnloadHandler();
        return;
    }
    // Short delay: arrowing through a folder shouldn't spin up a preview host for every file
    SetTimer(m_hwnd, kHandlerTimer, 150, nullptr);
}

void InfoPane::UnloadHandler()
{
    if (m_hwnd)
        KillTimer(m_hwnd, kHandlerTimer);
    if (m_handler)
    {
        m_handler->Unload();
        ComPtr<IObjectWithSite> site;
        if (SUCCEEDED(m_handler.As(&site)))
            site->SetSite(nullptr);
        m_handler.Reset();
    }
    if (m_host)
        ShowWindow(m_host, SW_HIDE);
}

void InfoPane::SetRich()
{
    if (!m_rich)
        return;
    SendMessageW(m_rich, WM_SETREDRAW, FALSE, 0);
    SendMessageW(m_rich, EM_SETTARGETDEVICE, 0, m_rtfWrap ? 0 : 1); // 0: wrap at the window, 1: no wrap
    SETTEXTEX st{ ST_DEFAULT, CP_ACP };
    SendMessageW(m_rich, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&st), reinterpret_cast<LPARAM>(m_rtf.c_str()));
    // A .rtf file's text in the automatic color would stay black on the dark pane: give those runs the theme's ink
    ComPtr<IRichEditOle> ole;
    ComPtr<ITextDocument> doc;
    if (m_rtfFile && m_theme->dark && SendMessageW(m_rich, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(ole.GetAddressOf())) && ole &&
        SUCCEEDED(ole.As(&doc)))
    {
        ComPtr<ITextRange> all;
        long length = 0;
        if (SUCCEEDED(doc->Range(0, 0, &all)))
            all->GetStoryLength(&length);
        for (long pos = 0, runs = 0; pos < length && runs < 20000; ++runs)
        {
            ComPtr<ITextRange> run;
            long end = 0;
            if (FAILED(doc->Range(pos, pos, &run)) || FAILED(run->MoveEnd(tomCharFormat, 1, nullptr)) || FAILED(run->GetEnd(&end)) || end <= pos)
                break;
            ComPtr<ITextFont> font;
            long color = 0;
            if (SUCCEEDED(run->GetFont(&font)) && SUCCEEDED(font->GetForeColor(&color)) && color == tomAutoColor)
                font->SetForeColor(static_cast<long>(m_theme->ink));
            pos = end;
        }
    }
    SendMessageW(m_rich, EM_SETSEL, 0, 0);
    SendMessageW(m_rich, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_rich, nullptr, TRUE);
}

void InfoPane::OpenLink(LONG first, LONG last)
{
    // The link's target (the HYPERLINK field), not its visible text
    ComPtr<IRichEditOle> ole;
    ComPtr<ITextDocument2> doc;
    ComPtr<ITextRange2> link;
    BSTR url = nullptr;
    std::wstring target;
    if (SendMessageW(m_rich, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(ole.GetAddressOf())) && ole && SUCCEEDED(ole.As(&doc)) &&
        SUCCEEDED(doc->Range2(first, last, &link)) && SUCCEEDED(link->GetURL(&url)) && url)
        target = url;
    SysFreeString(url);
    if (target.size() >= 2 && target.front() == L'"' && target.back() == L'"')
        target = target.substr(1, target.size() - 2);
    // Files hands it to Launcher.LaunchUriAsync; only web and mail links here, never a local program
    if (_wcsnicmp(target.c_str(), L"http://", 7) == 0 || _wcsnicmp(target.c_str(), L"https://", 8) == 0 || _wcsnicmp(target.c_str(), L"mailto:", 7) == 0)
        ShellExecuteW(m_hwnd, nullptr, target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// System preview handlers (Office, PDF, fonts, media, ...). Most run out of process in prevhost.exe.
void InfoPane::LoadHandler()
{
    UnloadHandler();
    if (!m_pidl || m_page != Page::Preview || m_kind != Kind::Handler)
        return;
    ComPtr<IPreviewHandler> handler;
    if (FAILED(CoCreateInstance(m_handlerClsid, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&handler))) &&
        FAILED(CoCreateInstance(m_handlerClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&handler))))
        handler.Reset();
    ComPtr<IShellItem> item;
    bool ready = false;
    if (handler && SUCCEEDED(SHCreateItemFromIDList(m_pidl.get(), IID_PPV_ARGS(&item))))
    {
        ComPtr<IInitializeWithItem> withItem;
        ComPtr<IInitializeWithStream> withStream;
        ComPtr<IInitializeWithFile> withFile;
        ComPtr<IStream> stream;
        PWSTR path = nullptr;
        // Explorer's order: stream first (works across the prevhost process boundary), then file path, then item
        if (SUCCEEDED(handler.As(&withStream)) && SUCCEEDED(item->BindToHandler(nullptr, BHID_Stream, IID_PPV_ARGS(&stream))))
            ready = SUCCEEDED(withStream->Initialize(stream.Get(), STGM_READ));
        if (!ready && SUCCEEDED(handler.As(&withFile)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            ready = SUCCEEDED(withFile->Initialize(path, STGM_READ));
        if (!ready && SUCCEEDED(handler.As(&withItem)))
            ready = SUCCEEDED(withItem->Initialize(item.Get(), STGM_READ));
        CoTaskMemFree(path);
        // Some handlers (Edge's HTML/PDF) query their site for IPreviewHandlerFrame before previewing
        ComPtr<IObjectWithSite> site;
        if (ready && SUCCEEDED(handler.As(&site)))
            site->SetSite(static_cast<IPreviewHandlerFrame*>(&m_frame));
        // Handlers that honour visuals (text, font, some code previewers) should blend with the pane.
        ComPtr<IPreviewHandlerVisuals> visuals;
        if (ready && SUCCEEDED(handler.As(&visuals)))
        {
            visuals->SetBackgroundColor(m_theme->content);
            visuals->SetTextColor(m_theme->ink);
        }
    }
    RECT rc{};
    GetClientRect(m_host, &rc);
    ShowWindow(m_host, SW_SHOWNA);
    if (ready && SUCCEEDED(handler->SetWindow(m_host, &rc)) && SUCCEEDED(handler->DoPreview()))
    {
        m_handler = handler;
        return;
    }
    if (handler)
        handler->Unload();
    ShowWindow(m_host, SW_HIDE);
    m_kind = Kind::None;
    m_note = Tr(L"此文件没有可用的预览");
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// --- Painting (blueprint .side-info: segmented 详情/预览 tabs, framed preview box, muted labels) ---

void InfoPane::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    RECT client;
    GetClientRect(m_hwnd, &client);
    FillSolid(hdc, rc, t.toolbar); // inside its card (App.Theme.InfoPane.BackgroundBrush)

    // Header: tabs + close
    const int top = t.S(8), bottom = t.S(kHeader - 8);
    const RECT tabs[3] = { { t.S(kPad), top, t.S(kPad + 56), bottom }, { t.S(kPad + 60), top, t.S(kPad + 116), bottom },
        { client.right - t.S(kPad + 28), top, client.right - t.S(kPad - 4), bottom } };
    for (int i = 0; i < 3; ++i)
    {
        const bool on = i < 2 && static_cast<int>(m_page) == i;
        if (on || i == m_hotTab)
            FillRound(hdc, tabs[i], t.S(5), on ? (i == m_hotTab ? Mix(t.selected, t.ink, 6) : t.selected) : t.hover);
        if (i < 2)
            DrawLabel(hdc, t.ui, i == 0 ? Tr(L"详情") : Tr(L"预览"), tabs[i], t.ink, DT_CENTER);
        else
            DrawGlyph(hdc, t.iconsSmall, Glyph::Close, tabs[i], t.muted);
        // The focus visual: on the checked page button (the pair's one stop), or on the close button
        const bool focused = m_ring && GetFocus() == m_hwnd &&
            ((m_focusPart == PartPages && on) || (m_focusPart == PartClose && i == 2));
        if (focused)
            DrawFocusVisual(hdc, t, tabs[i], t.S(5), on ? t.selected : t.toolbar, &client);
    }

    const RECT body = BodyRect();
    const int saved = SaveDC(hdc);
    IntersectClipRect(hdc, 0, body.top, client.right, client.bottom);
    int iconW = 16, iconH = 16;
    if (m_images)
        ImageList_GetIconSize(m_images, &iconW, &iconH);

    if (m_page == Page::Details)
    {
        // Preview box with the thumbnail (or a large icon), then name, type and the property grid
        const RECT box = ThumbRect();
        FillRound(hdc, box, t.S(6), t.content);
        FrameRound(hdc, box, t.S(6), t.line);
        const RECT inner{ box.left + t.S(8), box.top + t.S(8), box.right - t.S(8), box.bottom - t.S(8) };
        if (m_thumb)
            DrawFit(hdc, m_thumb, m_thumbAlpha, inner);
        else if (m_kind == Kind::Many)
            DrawGlyph(hdc, t.icons, L"\xE8A5", inner, t.muted);
        else if (m_icon >= 0 && m_images)
            ImageList_Draw(m_images, m_icon, hdc, (inner.left + inner.right - iconW) / 2, (inner.top + inner.bottom - iconH) / 2, ILD_TRANSPARENT);

        const RECT details = DetailsRect();
        int y = details.top;
        RECT name{ details.left, y, details.right, y + t.S(200) };
        if (DWriteText::Available())
        {
            name.bottom = name.top + DWriteText::WrappedHeight(hdc, m_bold, m_name, name.right - name.left);
            DWriteText::DrawWrapped(hdc, m_bold, m_name, name, t.ink, DT_LEFT);
        }
        else
        {
            HGDIOBJ old = SelectObject(hdc, m_bold);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, t.ink);
            DrawTextW(hdc, m_name.c_str(), -1, &name, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            name.right = details.right;
            DrawTextW(hdc, m_name.c_str(), -1, &name, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(hdc, old);
        }
        y = name.bottom + t.S(2);
        if (!m_type.empty())
        {
            DrawLabel(hdc, t.caption, m_type, { details.left, y, details.right, y + t.S(18) }, t.muted);
            y += t.S(18);
        }
        y += t.S(10);
        FillSolid(hdc, { details.left, y, details.right, y + std::max(1, t.S(1)) }, t.line);
        y += t.S(10);

        const int labelW = std::clamp(static_cast<int>(details.right - details.left) * 38 / 100, t.S(56), t.S(110));
        m_tagsRect = m_tagsButton = {};
        if (m_tagsShown && m_kind != Kind::Many)
        {
            // Files' details pane: the item's tags as colored chips; the row opens the tag menu
            const int chipH = t.S(22), gap = t.S(6), left = details.left + labelW + t.S(10);
            int x = left, rowY = y;
            auto chip = [&](const std::wstring& text, COLORREF color, bool add)
            {
                const int w = TextWidth(hdc, t.caption, text) + (add ? t.S(16) : t.S(28));
                if (x > left && x + w > details.right)
                {
                    x = left;
                    rowY += chipH + gap;
                }
                const RECT rc{ x, rowY, std::min<LONG>(x + w, details.right), rowY + chipH };
                if (add)
                    FrameRound(hdc, rc, chipH / 2, m_tagsHot ? t.accent : t.line);
                else
                {
                    FillRound(hdc, rc, chipH / 2, Mix(t.layer, color, t.dark ? 30 : 18));
                    const int d = t.S(8), cy = (rc.top + rc.bottom) / 2;
                    FillRound(hdc, { rc.left + t.S(9), cy - d / 2, rc.left + t.S(9) + d, cy - d / 2 + d }, d / 2, color);
                }
                DrawLabel(hdc, t.caption, text, { rc.left + (add ? t.S(8) : t.S(21)), rc.top, rc.right - t.S(6), rc.bottom },
                    add ? (m_tagsHot ? t.accent : t.muted) : t.ink);
                if (add)
                {
                    m_tagsButton = rc;
                    if (m_focusPart == PartTags && m_ring && GetFocus() == m_hwnd)
                        DrawFocusVisual(hdc, t, rc, chipH / 2, t.toolbar, &client);
                }
                x += w + gap;
            };
            for (const auto& [tagName, tagColor] : m_tags)
                chip(tagName, tagColor, false);
            chip(m_tags.empty() ? Tr(L"+ 添加标签") : Tr(L"编辑"), 0, true);
            const int h = rowY + chipH - y;
            DrawLabel(hdc, t.caption, Tr(L"标签"), { details.left, y, details.left + labelW, y + std::min(h, chipH) }, t.muted);
            m_tagsRect = { details.left, y, details.right, y + h };
            if (std::exchange(m_revealTags, false))
            {
                // Keyboard focus on the row: into view (the next paint shows it there)
                if (m_tagsButton.top < body.top)
                    PostMessageW(m_hwnd, kMsgReveal, 0, m_scroll - (body.top - m_tagsButton.top) - t.S(8));
                else if (m_tagsButton.bottom > body.bottom)
                    PostMessageW(m_hwnd, kMsgReveal, 0, m_scroll + (m_tagsButton.bottom - body.bottom) + t.S(8));
            }
            y += h + t.S(10);
        }
        std::vector<std::pair<std::wstring, std::wstring>> rows = m_props;
        rows.insert(rows.end(), m_extra.begin(), m_extra.end());
        for (const auto& [label, value] : rows)
        {
            const int valueWidth = details.right - (details.left + labelW + t.S(10));
            int vh;
            if (DWriteText::Available())
                vh = DWriteText::WrappedHeight(hdc, t.caption, value, valueWidth);
            else
            {
                HGDIOBJ old2 = SelectObject(hdc, t.caption);
                RECT v{ 0, 0, valueWidth, 0 };
                DrawTextW(hdc, value.c_str(), -1, &v, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(hdc, old2);
                vh = v.bottom;
            }
            const int h = std::max<int>(t.S(18), vh);
            DrawLabel(hdc, t.caption, label, { details.left, y, details.left + labelW, y + h }, t.muted, DT_LEFT);
            const RECT v{ details.left + labelW + t.S(10), y, details.right, y + h };
            if (DWriteText::Available())
                DWriteText::DrawWrapped(hdc, t.caption, value, v, t.ink, DT_LEFT);
            else
            {
                HGDIOBJ old2 = SelectObject(hdc, t.caption);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, t.ink);
                RECT vr = v;
                DrawTextW(hdc, value.c_str(), -1, &vr, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(hdc, old2);
            }
            y += h + t.S(8);
        }
        m_contentHeight = std::max<int>(y + m_scroll - body.top, Horizontal() ? ThumbRect().bottom - body.top : 0);
    }
    else
    {
        // Preview header: icon, name, type
        const RECT head{ body.left, body.top, body.right, body.top + t.S(kPreviewHead) - t.S(8) };
        int textX = head.left;
        if (m_kind != Kind::Many && m_icon >= 0 && m_images)
        {
            ImageList_Draw(m_images, m_icon, hdc, head.left, (head.top + head.bottom - iconH) / 2, ILD_TRANSPARENT);
            textX = head.left + iconW + t.S(10);
        }
        const int mid = (head.top + head.bottom) / 2;
        const RECT n{ textX, head.top, head.right, mid + t.S(2) };
        if (DWriteText::Available())
            DWriteText::DrawLabel(hdc, m_bold, m_name, n, t.ink, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS);
        else
        {
            SetBkMode(hdc, TRANSPARENT);
            HGDIOBJ old = SelectObject(hdc, m_bold);
            SetTextColor(hdc, t.ink);
            RECT nr = n;
            DrawTextW(hdc, m_name.c_str(), -1, &nr, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
        }
        DrawLabel(hdc, t.caption, m_note.empty() || m_kind != Kind::Text ? m_type : m_type + L" · " + m_note,
            { textX, mid + t.S(2), head.right, head.bottom }, t.muted);

        RECT area{ body.left, body.top + t.S(kPreviewHead), body.right, body.bottom };
        if (m_kind == Kind::Archive)
        {
            // Files' two rows: the item count, then 解压缩大小
            const size_t nl = m_archiveSummary.find(L'\n');
            DrawLabel(hdc, t.caption, m_archiveSummary.substr(0, nl), { area.left, area.top, area.right, area.top + t.S(20) }, t.muted, DT_END_ELLIPSIS);
            if (nl != std::wstring::npos)
                DrawLabel(hdc, t.caption, m_archiveSummary.substr(nl + 1), { area.left, area.top + t.S(20), area.right, area.top + t.S(40) }, t.muted,
                    DT_END_ELLIPSIS);
        }
        else if (m_kind == Kind::Image || (m_kind == Kind::None && m_thumb))
        {
            FillRound(hdc, area, t.S(6), t.content);
            FrameRound(hdc, area, t.S(6), t.line);
            DrawFit(hdc, m_thumb, m_thumbAlpha, { area.left + t.S(8), area.top + t.S(8), area.right - t.S(8), area.bottom - t.S(8) });
            if (m_kind == Kind::None && !m_note.empty())
                DrawLabel(hdc, t.caption, m_note, { area.left, area.bottom - t.S(40), area.right, area.bottom - t.S(16) }, t.muted, DT_CENTER);
        }
        else if (m_kind != Kind::Text && m_kind != Kind::Handler && m_kind != Kind::Rich && m_kind != Kind::Media)
        {
            FillRound(hdc, area, t.S(6), t.content);
            FrameRound(hdc, area, t.S(6), t.line);
            const std::wstring note = m_kind == Kind::Many ? Tr(L"选中多个项目时不显示预览") : m_note.empty() ? Tr(L"此文件没有可用的预览") : m_note;
            DrawLabel(hdc, t.caption, note, area, t.muted, DT_CENTER);
        }
        else if (m_kind == Kind::Handler && !m_handler)
        {
            DrawLabel(hdc, t.caption, Tr(L"正在加载预览…"), area, t.muted, DT_CENTER);
        }
    }
    RestoreDC(hdc, saved);
    UpdateBar(); // the content height is known once the details are laid out (here)
    m_bar.Paint(hdc, rc);
}

LRESULT CALLBACK InfoPane::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* self = reinterpret_cast<InfoPane*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<InfoPane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return self && self->m_hwnd == hwnd ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT InfoPane::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_SIZE:
    {
        LayoutChildren();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        // Re-render a thumbnail for the new size once resizing settles
        const SIZE size{ LOWORD(lp), HIWORD(lp) };
        if (m_pidl && (std::abs(size.cx - m_lastSize.cx) > m_theme->S(40) || std::abs(size.cy - m_lastSize.cy) > m_theme->S(40)))
        {
            m_lastSize = size;
            SetTimer(m_hwnd, kResizeTimer, 250, nullptr);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        if (const bool held = m_bar.Captured(); m_bar.OnMouseMove(pt, (wp & MK_LBUTTON) != 0) || held)
        {
            if (m_tagsHot)
            {
                m_tagsHot = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (const int hot = HitTab(pt); hot != m_hotTab)
        {
            m_hotTab = hot;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (const bool tagsHot = m_page == Page::Details && PtInRect(&m_tagsRect, pt) != FALSE; tagsHot != m_tagsHot)
        {
            m_tagsHot = tagsHot;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (m_tagsHot && LOWORD(lp) == HTCLIENT)
        {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_bar.OnMouseLeave();
        m_hotTab = -1;
        m_tagsHot = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (m_ring)
        {
            m_ring = false; // a pointer press hides the keyboard focus visual (WinUI FocusState.Pointer)
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        if (m_bar.Over(pt))
            m_bar.OnButtonDown(pt);
        return 0;
    case WM_GETDLGCODE:
        return GetFocus() == m_hwnd ? DLGC_WANTARROWS | DLGC_WANTCHARS : 0;
    case WM_KEYDOWN:
        if (GetFocus() != m_hwnd)
            break;
        if (m_focusPart == PartPages && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN))
        {
            // RadioButtons: the arrows move the check (and the focus) to the other page
            const Page to = (wp == VK_LEFT || wp == VK_UP) ? Page::Details : Page::Preview;
            m_ring = true;
            SetPage(to);
            NameFocus();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        if ((wp == VK_RETURN || wp == VK_SPACE) && m_focusPart >= 0)
        {
            Press(m_focusPart);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case kMsgReveal:
        m_smooth.Reset(static_cast<int>(lp));
        ScrollTo(static_cast<int>(lp));
        return 0;
    case WM_CAPTURECHANGED:
        m_bar.OnCaptureLost();
        return 0;
    case WM_LBUTTONUP:
        if (m_bar.OnButtonUp() || m_bar.Over(pt))
            return 0;
        if (m_page == Page::Details && PtInRect(&m_tagsRect, pt))
        {
            POINT screen = pt;
            ClientToScreen(m_hwnd, &screen);
            m_events->OnInfoPaneTags(screen);
            return 0;
        }
        switch (HitTab(pt))
        {
        case 0: SetPage(Page::Details); break;
        case 1: SetPage(Page::Preview); break;
        case 2: m_events->OnInfoPaneClose(); break;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (m_page == Page::Details)
        {
            const RECT body = BodyRect();
            const int maxScroll = std::max(0, m_contentHeight - static_cast<int>(body.bottom - body.top));
            ScrollTo(m_smooth.OnWheel(m_hwnd, GET_WHEEL_DELTA_WPARAM(wp), m_theme->S(48), maxScroll));
        }
        return 0;
    case SmoothFrame::kMsgTick:
        if (m_smooth.Animating())
            ScrollTo(m_smooth.Tick());
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), m_theme->ink);
        SetBkColor(reinterpret_cast<HDC>(wp), m_theme->content);
        return reinterpret_cast<LRESULT>(m_contentBrush);
    case WM_NOTIFY:
        // Files' MarkdownPreview OnLinkClicked: open the link
        if (auto* link = reinterpret_cast<ENLINK*>(lp); m_rich && link->nmhdr.hwndFrom == m_rich && link->nmhdr.code == EN_LINK)
        {
            if (link->msg == WM_LBUTTONUP)
                OpenLink(link->chrg.cpMin, link->chrg.cpMax);
            return 0;
        }
        if (auto* info = reinterpret_cast<NMLVDISPINFOW*>(lp); info->hdr.hwndFrom == m_list && info->hdr.code == LVN_GETDISPINFOW)
        {
            const int i = info->item.iItem;
            if (i >= 0 && i < static_cast<int>(m_archive.size()))
            {
                const ArchiveEntry& e = m_archive[i];
                if (info->item.mask & LVIF_TEXT)
                {
                    const std::wstring text = info->item.iSubItem == 0 ? e.name : e.folder ? L"" : FormatBytes(e.size);
                    wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
                }
                if (info->item.mask & LVIF_IMAGE)
                    info->item.iImage = e.icon;
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == kBarTimer)
        {
            m_bar.OnTimer(wp);
            return 0;
        }
        KillTimer(m_hwnd, wp);
        if (wp == kHandlerTimer)
            LoadHandler();
        else if (wp == kResizeTimer && m_kind != Kind::Handler && m_kind != Kind::Many && !(m_page == Page::Preview && (m_kind == Kind::Rich ||
                     m_kind == Kind::Media || m_kind == Kind::Text || m_kind == Kind::Archive)))
            Start(); // a thumbnail for the new size (text, lists and the player already fit)
        return 0;
    case kMsgResult:
        OnResult(reinterpret_cast<Result*>(lp));
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}