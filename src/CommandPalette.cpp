#include "CommandPalette.h"
#include "Lang.h"

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.Palette";
constexpr int kRow = 36, kMaxRows = 10, kPad = 4;

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
}

CommandPalette::~CommandPalette()
{
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}

bool CommandPalette::Create(HWND owner, const Theme* theme)
{
    static const bool registered = []
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    m_owner = owner;
    m_theme = theme;
    // Never activated: the omnibar edit keeps the focus and the caret while the list follows the typing
    m_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
        GetModuleHandleW(nullptr), this);
    return registered && m_hwnd;
}

void CommandPalette::Show(const RECT& below)
{
    m_below = below;
    m_selected = 0;
    m_scroll = 0;
    m_smooth.Reset(0);
    m_hot = -1;
    Filter(L"");
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void CommandPalette::Hide()
{
    if (m_hwnd)
        ShowWindow(m_hwnd, SW_HIDE);
}

void CommandPalette::Filter(const std::wstring& text)
{
    const std::wstring query = Lower(text);
    m_shown.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
    {
        const Entry& e = m_all[i];
        if (query.empty() || Lower(e.name).find(query) != std::wstring::npos || Lower(e.category).find(query) != std::wstring::npos ||
            Lower(e.keys).find(query) != std::wstring::npos)
            m_shown.push_back(static_cast<int>(i));
    }
    m_selected = 0;
    m_scroll = 0;
    m_smooth.Reset(0);
    Resize();
}

void CommandPalette::Move(int delta)
{
    const int n = static_cast<int>(m_shown.size());
    if (n == 0)
        return;
    m_selected = std::clamp(m_selected + delta, 0, n - 1);
    EnsureVisible();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int CommandPalette::Selected() const
{
    return m_selected >= 0 && m_selected < static_cast<int>(m_shown.size()) ? m_all[m_shown[m_selected]].id : 0;
}

void CommandPalette::Relayout()
{
    if (Visible())
        Resize();
}

void CommandPalette::Resize()
{
    const Theme& t = *m_theme;
    const int rows = std::max(1, std::min(static_cast<int>(m_shown.size()), kMaxRows));
    const int h = rows * t.S(kRow) + t.S(kPad) * 2 + 2;
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_below.left, m_below.bottom + t.S(4), m_below.right - m_below.left, h, SWP_NOACTIVATE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CommandPalette::EnsureVisible()
{
    if (m_selected < m_scroll)
        m_scroll = m_selected;
    else if (m_selected >= m_scroll + kMaxRows)
        m_scroll = m_selected - kMaxRows + 1;
    else
        return;
    m_smooth.Reset(m_scroll); // arrow-key navigation jumps straight there, like a native ListBox
}

int CommandPalette::RowAt(int y) const
{
    const int row = (y - m_theme->S(kPad) - 1) / m_theme->S(kRow) + m_scroll;
    return y > m_theme->S(kPad) && row >= m_scroll && row < static_cast<int>(m_shown.size()) ? row : -1;
}

void CommandPalette::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.line);
    FillSolid(hdc, { 1, 1, rc.right - 1, rc.bottom - 1 }, t.layer);
    if (m_shown.empty())
    {
        DrawLabel(hdc, t.ui, Tr(L"没有匹配的命令"), { t.S(16), t.S(kPad), rc.right - t.S(16), t.S(kPad) + t.S(kRow) }, t.muted);
        return;
    }
    const int last = std::min(static_cast<int>(m_shown.size()), m_scroll + kMaxRows);
    for (int i = m_scroll; i < last; ++i)
    {
        const Entry& e = m_all[m_shown[i]];
        const int top = t.S(kPad) + 1 + (i - m_scroll) * t.S(kRow);
        RECT r{ t.S(kPad) + 1, top, rc.right - t.S(kPad) - 1, top + t.S(kRow) };
        if (i == m_selected || i == m_hot)
            FillRound(hdc, r, t.S(5), i == m_selected ? t.selected : t.hover);
        if (i == m_selected)
            FillRound(hdc, { r.left, r.top + t.S(10), r.left + t.S(3), r.bottom - t.S(10) }, t.S(2), t.accent);
        // Shortcut chips on the right, then category, name on the left
        int right = r.right - t.S(10);
        std::wstring keys = e.keys;
        std::vector<std::wstring> chips;
        for (size_t p = 0; !keys.empty();)
        {
            p = keys.find(L"  ");
            chips.push_back(keys.substr(0, p));
            keys = p == std::wstring::npos ? L"" : keys.substr(p + 2);
        }
        for (auto it = chips.rbegin(); it != chips.rend(); ++it)
        {
            const int w = TextWidth(hdc, t.caption, *it) + t.S(12);
            const RECT chip{ right - w, r.top + t.S(8), right, r.bottom - t.S(8) };
            FrameRound(hdc, chip, t.S(4), t.line);
            DrawLabel(hdc, t.caption, *it, chip, t.muted, DT_CENTER);
            right = chip.left - t.S(6);
        }
        const int nameW = TextWidth(hdc, t.ui, e.name);
        const RECT name{ r.left + t.S(14), r.top, std::min<LONG>(right, r.left + t.S(14) + nameW), r.bottom };
        DrawLabel(hdc, t.ui, e.name, name, t.ink);
        DrawLabel(hdc, t.caption, e.category, { name.right + t.S(10), r.top, right - t.S(4), r.bottom }, t.muted);
    }
}

LRESULT CALLBACK CommandPalette::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* created = static_cast<CommandPalette*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->m_hwnd = hwnd; // DefWindowProc below needs the handle while CreateWindowEx is still running
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<CommandPalette*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CommandPalette::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
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
        const int row = RowAt(GET_Y_LPARAM(lp));
        if (row != m_hot)
        {
            m_hot = row;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_hot = -1;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (const int row = RowAt(GET_Y_LPARAM(lp)); row >= 0)
            PostMessageW(m_owner, kMsgRun, static_cast<WPARAM>(m_all[m_shown[row]].id), 0);
        return 0;
    case WM_MOUSEWHEEL:
    {
        const int n = static_cast<int>(m_shown.size());
        m_scroll = m_smooth.OnWheel(m_hwnd, GET_WHEEL_DELTA_WPARAM(wp), 3, std::max(0, n - kMaxRows));
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return 0;
    }
    case SmoothFrame::kMsgTick:
        if (m_smooth.Animating())
        {
            m_scroll = m_smooth.Tick();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
