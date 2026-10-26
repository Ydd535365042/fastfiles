#include "Controls.h"

namespace
{
constexpr wchar_t kBarClass[] = L"FastFiles.ButtonBar";
constexpr wchar_t kStatusClass[] = L"FastFiles.StatusBar";

template <class T>
T* Attach(HWND hwnd, UINT msg, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    return reinterpret_cast<T*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

bool Register(const wchar_t* name, WNDPROC proc)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
}

bool ButtonBar::Create(HWND parent, int id, const Theme* theme, ButtonBarEvents* events, std::vector<Item> items, const COLORREF* background)
{
    static const bool registered = Register(kBarClass, WndProc);
    if (!registered)
        return false;
    m_theme = theme;
    m_events = events;
    m_items = std::move(items);
    m_background = background;
    m_hwnd = CreateWindowExW(0, kBarClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, MenuId(id),
        GetModuleHandleW(nullptr), this);
    if (!m_hwnd)
        return false;
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].id <= 0 || m_items[i].tip.empty())
            continue;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = m_hwnd;
        ti.uId = i + 1;
        ti.lpszText = m_items[i].tip.data(); // copied by the control
        SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    return true;
}

void ButtonBar::SetItems(std::vector<Item> items)
{
    if (m_tooltip)
        for (size_t i = 0; i < m_items.size(); ++i)
        {
            TTTOOLINFOW ti{ sizeof(ti) };
            ti.hwnd = m_hwnd;
            ti.uId = i + 1;
            SendMessageW(m_tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    m_items = std::move(items);
    m_hot = m_pressed = m_flash = -1;
    for (size_t i = 0; m_tooltip && i < m_items.size(); ++i)
    {
        if (m_items[i].id <= 0 || m_items[i].tip.empty())
            continue;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = m_hwnd;
        ti.uId = i + 1;
        ti.lpszText = m_items[i].tip.data();
        SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    Relayout();
}

int ButtonBar::ItemAt(POINT client) const
{
    const int index = HitTest(client);
    return index >= 0 ? m_items[index].id : 0;
}

ButtonBar::Item* ButtonBar::Find(int id)
{
    for (auto& item : m_items)
        if (item.id == id)
            return &item;
    return nullptr;
}

void ButtonBar::SetEnabled(int id, bool enabled)
{
    if (Item* item = Find(id); item && item->enabled != enabled)
    {
        item->enabled = enabled;
        InvalidateRect(m_hwnd, &item->rc, FALSE);
    }
}

void ButtonBar::SetChecked(int id, bool checked)
{
    if (Item* item = Find(id); item && item->checked != checked)
    {
        item->checked = checked;
        InvalidateRect(m_hwnd, &item->rc, FALSE);
    }
}

void ButtonBar::SetGlyph(int id, const wchar_t* glyph)
{
    if (Item* item = Find(id))
    {
        item->glyph = glyph;
        InvalidateRect(m_hwnd, &item->rc, FALSE);
    }
}

void ButtonBar::SetVisible(int id, bool visible)
{
    if (Item* item = Find(id); item && item->visible != visible)
    {
        item->visible = visible;
        Relayout();
    }
}

RECT ButtonBar::ScreenRect(int id) const
{
    for (const auto& item : m_items)
        if (item.id == id)
        {
            RECT rc = item.rc;
            MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
            return rc;
        }
    return {};
}

bool ButtonBar::IsEnabled(int id) const
{
    for (const auto& item : m_items)
        if (item.id == id)
            return item.enabled;
    return false;
}

int ButtonBar::ItemWidth(HDC hdc, const Item& item, bool compact) const
{
    const Theme& t = *m_theme;
    if (!item.visible)
        return 0;
    if (item.id == 0)
        return t.S(9);
    if (item.id < 0)
        return 0;
    const bool label = !item.label.empty() && !(compact && item.glyph);
    int w = label ? t.S(10) + (item.glyph ? t.S(16) + t.S(8) : 0) + TextWidth(hdc, t.ui, item.label) + t.S(10) : t.S(36);
    if (item.dropdown)
        w += label ? t.S(14) : t.S(12);
    return w;
}

std::vector<bool> ButtonBar::Shown() const
{
    std::vector<bool> shown(m_items.size());
    // A separator shows only between two visible buttons; spacers always count as a boundary
    bool buttonBefore = false;
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        const Item& item = m_items[i];
        if (!item.visible)
            continue;
        if (item.id > 0)
        {
            shown[i] = true;
            buttonBefore = true;
        }
        else if (item.id < 0)
        {
            shown[i] = true;
            buttonBefore = false;
        }
        else if (buttonBefore)
        {
            bool buttonAfter = false;
            for (size_t j = i + 1; j < m_items.size() && m_items[j].id >= 0 && !buttonAfter; ++j)
                buttonAfter = m_items[j].visible && m_items[j].id > 0;
            shown[i] = buttonAfter;
            buttonBefore = !buttonAfter; // two separators in a row collapse into one
        }
    }
    return shown;
}

int ButtonBar::IdealWidth()
{
    HDC hdc = GetDC(m_hwnd);
    int w = m_theme->S(4) * 2;
    const auto shown = Shown();
    for (size_t i = 0; i < m_items.size(); ++i)
        if (shown[i])
            w += ItemWidth(hdc, m_items[i], false) + m_theme->S(2);
    ReleaseDC(m_hwnd, hdc);
    return w;
}

void ButtonBar::Relayout()
{
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const Theme& t = *m_theme;
    const int gap = t.S(2), pad = t.S(4);
    const int h = std::min(static_cast<int>(rc.bottom), t.S(32));
    const int top = (rc.bottom - h) / 2;
    HDC hdc = GetDC(m_hwnd);
    const auto shown = Shown();
    auto total = [&](bool compact)
    {
        int w = pad * 2;
        for (size_t i = 0; i < m_items.size(); ++i)
            if (shown[i])
                w += ItemWidth(hdc, m_items[i], compact) + gap;
        return w;
    };
    // Narrow windows drop labels (icons only), like Files' collapsed toolbar
    const bool compact = total(false) > rc.right;
    int spare = std::max(0, static_cast<int>(rc.right) - total(compact));
    int x = pad;
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        Item& item = m_items[i];
        int w = ItemWidth(hdc, item, compact);
        if (item.id < 0)
        {
            w = spare;
            spare = 0;
        }
        if (!shown[i])
        {
            item.rc = {};
        }
        else
        {
            item.rc = { x, top, x + w, top + h };
            x += w + gap;
        }
        if (m_tooltip && item.id > 0)
        {
            TTTOOLINFOW ti{ sizeof(ti) };
            ti.hwnd = m_hwnd;
            ti.uId = i + 1;
            ti.rect = item.rc;
            SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
    ReleaseDC(m_hwnd, hdc);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int ButtonBar::HitTest(POINT pt) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id > 0 && m_items[i].visible && PtInRect(&m_items[i].rc, pt))
            return static_cast<int>(i);
    return -1;
}

void ButtonBar::Fire(int index)
{
    const Item& item = m_items[index];
    if (!item.enabled)
        return;
    if (item.dropdown && m_events)
    {
        RECT screen = item.rc;
        MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
        m_events->OnBarDropdown(item.id, screen);
        return;
    }
    SendMessageW(GetParent(m_hwnd), WM_COMMAND, MAKEWPARAM(item.id, BN_CLICKED), reinterpret_cast<LPARAM>(m_hwnd));
}

void ButtonBar::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    const COLORREF background = *m_background;
    FillSolid(hdc, rc, background);
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        const Item& item = m_items[i];
        const RECT& r = item.rc;
        if (!item.visible || IsRectEmpty(&r))
            continue;
        if (item.id == 0)
        {
            const int x = (r.left + r.right) / 2;
            FillSolid(hdc, { x, r.top + t.S(8), x + std::max(1, t.S(1)), r.bottom - t.S(8) }, t.line);
            continue;
        }
        if (item.id < 0)
            continue;
        const bool hot = item.enabled && static_cast<int>(i) == m_hot;
        const bool down = item.enabled && (static_cast<int>(i) == m_pressed || static_cast<int>(i) == m_flash);
        if (m_sidebarLook)
        {
            // Files' footer SettingsButton is a SidebarItem: the same states as Sidebar::Paint's rows (subtle fills
            // for hover/press, the raised control fill + stroke and the accent pill while Settings is open)
            const int radius = t.S(4);
            if (item.checked)
            {
                FillRound(hdc, r, radius, down ? t.controlFillPressed : hot ? t.controlFillHover : t.controlFill);
                FrameRound(hdc, r, radius, t.controlStroke);
                const int cy = (r.top + r.bottom) / 2;
                FillRound(hdc, { r.left + t.S(1), cy - t.S(8), r.left + t.S(4), cy + t.S(8) }, t.S(2), t.accent);
            }
            else if (down || hot)
                FillRound(hdc, r, radius, down ? t.subtlePressed : t.subtleHover);
        }
        else if (m_buttonLook)
        {
            // Files' AddressToolbarButtonStyle: a fill and the elevation border under the mouse, a deeper fill (no
            // border) while pressed
            if (down)
                FillRound(hdc, r, t.S(4), Mix(background, t.ink, t.buttonPressedPct));
            else if (hot)
            {
                FillRound(hdc, r, t.S(4), Mix(background, t.ink, t.buttonHoverPct));
                FrameRound(hdc, r, t.S(4), t.controlStroke);
            }
        }
        else if (item.checked && !down)
            FillRound(hdc, r, t.S(5), hot ? Mix(t.selected, t.ink, 6) : t.selected);
        else if (down || hot)
            FillRound(hdc, r, t.S(5), Mix(background, t.ink, down ? t.buttonPressedPct : t.buttonHoverPct)); // AppBarButton states
        // ButtonForegroundPressed / AppBarButtonForegroundPressed: TextFillColorSecondary (the toolbar's buttons too)
        const COLORREF ink = !item.enabled ? Mix(t.ink, background, 62)
            : down && !m_sidebarLook ? Mix(background, t.secondaryText, t.secondaryTextPct) : t.ink;
        const bool label = !item.label.empty() && (r.right - r.left) > t.S(48);
        int x = r.left + (label ? t.S(10) : 0);
        if (item.glyph)
        {
            const RECT g = label ? RECT{ x, r.top, x + t.S(16), r.bottom } : RECT{ r.left, r.top, r.right - (item.dropdown ? t.S(12) : 0), r.bottom };
            DrawGlyph(hdc, t.icons, item.glyph, g, ink);
            x = g.right + t.S(8);
        }
        if (label)
            DrawLabel(hdc, t.ui, item.label, { x, r.top, r.right - (item.dropdown ? t.S(20) : t.S(6)), r.bottom }, ink);
        if (item.dropdown)
            DrawGlyph(hdc, t.iconsSmall, Glyph::ChevronDown, { r.right - t.S(label ? 22 : 16), r.top, r.right - t.S(label ? 6 : 2), r.bottom }, Mix(ink, background, 25));
    }
}

LRESULT CALLBACK ButtonBar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = Attach<ButtonBar>(hwnd, msg, lp);
    if (self && msg == WM_NCCREATE)
        self->m_hwnd = hwnd;
    return self && self->m_hwnd == hwnd ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ButtonBar::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    switch (msg)
    {
    case WM_SIZE:
        Relayout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        BufferedPaint paint(m_hwnd);
        Paint(paint.dc(), paint.rect());
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!m_tracking)
        {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        const int hot = HitTest(pt);
        if (hot != m_hot)
        {
            m_hot = hot;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_tracking = false;
        m_hot = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        const int index = HitTest(pt);
        if (index < 0 || !m_items[index].enabled)
            return 0;
        m_pressed = index;
        m_pressedAt = GetTickCount64();
        KillTimer(m_hwnd, kFlashTimer);
        m_flash = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        UpdateWindow(m_hwnd); // the pressed look now, not after whatever the click runs
        if (m_items[index].dropdown)
        {
            UpdateWindow(m_hwnd);
            Fire(index); // modal menu
            m_pressed = -1;
            m_hot = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        SetCapture(m_hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == kFlashTimer)
        {
            KillTimer(m_hwnd, kFlashTimer);
            m_flash = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        const int pressed = m_pressed;
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        m_pressed = -1;
        // A quick click still shows its pressed look for a moment (the button-up can come before the screen updated)
        if (pressed >= 0 && GetTickCount64() - m_pressedAt < kFlashMs)
        {
            m_flash = pressed;
            SetTimer(m_hwnd, kFlashTimer, static_cast<UINT>(kFlashMs - (GetTickCount64() - m_pressedAt)), nullptr);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        // Dropdowns already opened on the press; a late button-up (after the menu, or a dialog it led to) must not reopen them
        if (pressed >= 0 && pressed < static_cast<int>(m_items.size()) && !m_items[pressed].dropdown && HitTest(pt) == pressed)
            Fire(pressed);
        return 0;
    }
    case WM_RBUTTONUP:
        if (!m_events)
            return 0;
        if (const int index = HitTest(pt); index >= 0)
        {
            RECT screen = m_items[index].rc;
            MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&screen), 2);
            m_events->OnBarContext(m_items[index].id, screen);
        }
        else
        {
            // Empty part of the bar (Files: the toolbar's own right-click menu): id 0, the point as an empty rect
            POINT screen = pt;
            ClientToScreen(m_hwnd, &screen);
            m_events->OnBarContext(0, { screen.x, screen.y, screen.x, screen.y });
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (m_pressed >= 0 && !m_items[m_pressed].dropdown)
        {
            m_pressed = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        if (m_tooltip)
            DestroyWindow(m_tooltip);
        m_tooltip = nullptr;
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

bool StatusBar::Create(HWND parent, int id, const Theme* theme)
{
    static const bool registered = Register(kStatusClass, WndProc);
    m_theme = theme;
    m_hwnd = registered ? CreateWindowExW(0, kStatusClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, MenuId(id),
        GetModuleHandleW(nullptr), this) : nullptr;
    if (!m_hwnd)
        return false;
    m_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        0, 0, 0, 0, m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (UINT_PTR i = 0; m_tooltip && i < kChips; ++i)
    {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = m_hwnd;
        ti.uId = i;
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    return true;
}

void StatusBar::SetSync(const std::wstring& text, const std::wstring& tip)
{
    Chip& c = m_chips[kSync];
    if (text == c.text && tip == c.tip)
        return;
    c.text = text;
    c.tip = tip;
    c.glyph = Glyph::Sync;
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK StatusBar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = Attach<StatusBar>(hwnd, msg, lp);
    switch (msg)
    {
    case WM_SETTEXT:
    {
        const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return r;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (self)
        {
            const int hot = self->HitChip({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (!self->m_tracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                self->m_tracking = TrackMouseEvent(&tme) != FALSE;
            }
            if (hot != self->m_hot)
            {
                self->m_hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, hot >= 0 ? IDC_HAND : IDC_ARROW));
        }
        return 0;
    case WM_NOTIFY:
        if (self && reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW)
        {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
            const UINT_PTR chip = info->hdr.idFrom;
            info->lpszText = chip < kChips && !self->m_chips[chip].text.empty() && !self->m_chips[chip].tip.empty()
                ? const_cast<wchar_t*>(self->m_chips[chip].tip.c_str()) : const_cast<wchar_t*>(L"");
            return 0;
        }
        break;
    case WM_DESTROY:
        if (self && self->m_tooltip)
        {
            DestroyWindow(self->m_tooltip);
            self->m_tooltip = nullptr;
        }
        break;
    case WM_MOUSELEAVE:
        if (self)
        {
            self->m_tracking = false;
            if (self->m_hot >= 0)
            {
                self->m_hot = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (self)
        {
            const int chip = self->HitChip({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
            if (chip >= 0)
                SendMessageW(GetParent(hwnd), WM_COMMAND,
                    MAKEWPARAM(GetDlgCtrlID(hwnd), chip == kBranch ? kBranchClick : kSyncClick),
                    reinterpret_cast<LPARAM>(hwnd));
        }
        return 0;
    case WM_PAINT:
        if (self && self->m_theme)
        {
            BufferedPaint paint(hwnd);
            self->Paint(paint.dc(), paint.rect());
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void StatusBar::SetBranch(const std::wstring& branch)
{
    Chip& c = m_chips[kBranch];
    if (branch == c.text)
        return;
    c.text = branch;
    c.glyph = Glyph::Branch;
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

int StatusBar::HitChip(POINT pt) const
{
    for (int i = 0; i < kChips; ++i)
        if (!m_chips[i].text.empty() && PtInRect(&m_chips[i].rc, pt))
            return i;
    return -1;
}

RECT StatusBar::ChipScreenRect(int chip) const
{
    RECT rc = m_chips[chip].rc;
    MapWindowPoints(m_hwnd, HWND_DESKTOP, reinterpret_cast<POINT*>(&rc), 2);
    return rc;
}

void StatusBar::Paint(HDC hdc, RECT rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.content);
    FillSolid(hdc, { rc.left, rc.top, rc.right, rc.top + std::max(1, t.S(1)) }, t.line);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(m_hwnd)) + 1, L'\0');
    text.resize(GetWindowTextW(m_hwnd, text.data(), static_cast<int>(text.size())));
    const size_t tab = text.find(L'\t');
    rc.left += t.S(12);
    rc.right -= t.S(12);
    // Chips from the right edge: branch, then sync; the status text ends before them
    for (int i = kChips - 1; i >= 0; --i)
    {
        Chip& c = m_chips[i];
        c.rc = {};
        if (c.text.empty())
            continue;
        const int glyphW = t.S(16), pad = t.S(8);
        const int textW = std::min<int>(TextWidth(hdc, t.caption, c.text), t.S(260));
        const int h = std::min<int>(t.S(24), rc.bottom - rc.top - t.S(4));
        const int top = (rc.top + rc.bottom - h) / 2 + std::max(1, t.S(1)) / 2;
        c.rc = { rc.right - (pad + glyphW + t.S(4) + textW + pad), top, rc.right, top + h };
        const bool hot = m_hot == i;
        if (hot)
            FillRound(hdc, c.rc, t.S(4), t.hover);
        RECT glyph{ c.rc.left + pad, c.rc.top, c.rc.left + pad + glyphW, c.rc.bottom };
        DrawGlyph(hdc, t.iconsMid, c.glyph, glyph, t.muted);
        const int x = glyph.right + t.S(4);
        DrawLabel(hdc, t.caption, c.text, { x, c.rc.top, c.rc.right - pad, c.rc.bottom }, hot ? t.ink : t.muted);
        rc.right = c.rc.left - t.S(6);
    }
    // Tooltips follow the chips (hidden chips get an empty rect)
    for (UINT_PTR i = 0; m_tooltip && i < kChips; ++i)
    {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = m_hwnd;
        ti.uId = i;
        ti.rect = m_chips[i].rc;
        SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
    rc.right -= t.S(6);
    DrawLabel(hdc, t.caption, text.substr(0, tab), rc, t.muted);
    if (tab != std::wstring::npos)
        DrawLabel(hdc, t.caption, text.substr(tab + 1), rc, t.muted, DT_RIGHT);
}