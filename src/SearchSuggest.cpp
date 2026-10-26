#include "SearchSuggest.h"
#include "Lang.h"
#include "fastfiles_core.h"
#include <mutex>
#include <thread>

namespace
{
constexpr wchar_t kClass[] = L"FastFiles.SearchSuggest";
constexpr int kRow = 36, kPad = 4;
constexpr size_t kMax = 10; // Files' MaxSuggestionsCount / MaxItemCount
}

SearchSuggest::~SearchSuggest()
{
    if (m_cancel)
        *m_cancel = true;
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}

bool SearchSuggest::Create(HWND owner, const Theme* theme)
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
    // Never activated: the search box keeps the focus and the caret while the list follows the typing
    m_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
        GetModuleHandleW(nullptr), this);
    return registered && m_hwnd;
}

void SearchSuggest::Show(const RECT& below, std::vector<Entry> entries)
{
    m_below = below;
    m_entries = std::move(entries);
    m_selected = -1; // like Files: nothing highlighted until the arrow keys move (Enter searches the text)
    m_hot = -1;
    // The window text lists the rows ("kind<TAB>text" per line) so tests can read them
    std::wstring text;
    for (const auto& e : m_entries)
        text += (e.kind == Kind::Query ? L"query\t" : e.kind == Kind::Recent ? L"recent\t" : L"item\t") + e.text + L"\n";
    SetWindowTextW(m_hwnd, text.c_str());
    if (m_entries.empty())
    {
        Hide();
        return;
    }
    Resize();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void SearchSuggest::Hide()
{
    if (m_cancel)
        *m_cancel = true;
    if (m_hwnd)
        ShowWindow(m_hwnd, SW_HIDE);
}

void SearchSuggest::Move(int delta)
{
    const int n = static_cast<int>(m_entries.size());
    if (n == 0)
        return;
    m_selected = m_selected < 0 ? (delta > 0 ? 0 : n - 1) : std::clamp(m_selected + delta, 0, n - 1);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

const SearchSuggest::Entry* SearchSuggest::Selected() const
{
    return m_selected >= 0 && m_selected < static_cast<int>(m_entries.size()) ? &m_entries[m_selected] : nullptr;
}

void SearchSuggest::Relayout()
{
    if (Visible())
        Resize();
}

void SearchSuggest::Resize()
{
    const Theme& t = *m_theme;
    const int rows = std::max(1, std::min(static_cast<int>(m_entries.size()), static_cast<int>(kMax) + 1));
    const int h = rows * t.S(kRow) + t.S(kPad) * 2 + 2;
    const int w = std::max<int>(m_below.right - m_below.left, t.S(320));
    SetWindowPos(m_hwnd, HWND_TOPMOST, m_below.right - w, m_below.bottom + t.S(4), w, h, SWP_NOACTIVATE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

int SearchSuggest::RowAt(int y) const
{
    const int row = (y - m_theme->S(kPad) - 1) / m_theme->S(kRow);
    return y > m_theme->S(kPad) && row >= 0 && row < static_cast<int>(m_entries.size()) ? row : -1;
}

void SearchSuggest::Paint(HDC hdc, const RECT& rc)
{
    const Theme& t = *m_theme;
    FillSolid(hdc, rc, t.line);
    FillSolid(hdc, { 1, 1, rc.right - 1, rc.bottom - 1 }, t.layer);
    HIMAGELIST icons = nullptr;
    SHFILEINFOW sfi{};
    icons = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(L"", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON));
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
    {
        const Entry& e = m_entries[i];
        const int top = t.S(kPad) + 1 + i * t.S(kRow);
        RECT r{ t.S(kPad) + 1, top, rc.right - t.S(kPad) - 1, top + t.S(kRow) };
        if (i == m_selected || i == m_hot)
            FillRound(hdc, r, t.S(5), i == m_selected ? t.selected : t.hover);
        if (i == m_selected)
            FillRound(hdc, { r.left, r.top + t.S(10), r.left + t.S(3), r.bottom - t.S(10) }, t.S(2), t.accent);
        // Files' SuggestionModel.PlaceholderGlyph: search (the query), history (a previous search), else the item's icon
        RECT glyph{ r.left + t.S(12), r.top, r.left + t.S(12) + t.S(16), r.bottom };
        if (e.kind == Kind::Item && e.icon >= 0 && icons)
        {
            const int size = GetSystemMetrics(SM_CXSMICON);
            ImageList_Draw(icons, e.icon, hdc, glyph.left + (t.S(16) - size) / 2, (r.top + r.bottom - size) / 2, ILD_TRANSPARENT);
        }
        else
            DrawGlyph(hdc, t.iconsMid, e.kind == Kind::Query ? L"\xE721" : e.kind == Kind::Recent ? L"\xE81C" : L"\xE7C3", glyph, t.muted);
        // Files' DisplayName: "Search for `{0}`" for the query row
        const std::wstring label = e.kind == Kind::Query ? Lang::Format(L"搜索“{0}”", { e.text }) : e.text;
        DrawLabel(hdc, t.ui, label, { glyph.right + t.S(12), r.top, r.right - t.S(10), r.bottom }, t.ink);
    }
}

void SearchSuggest::FindItems(const std::wstring& folder, const std::wstring& text, bool hidden, unsigned request)
{
    if (m_cancel)
        *m_cancel = true;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread([owner = m_owner, folder, text, hidden, request, cancel = m_cancel]
    {
        // Files waits 200 ms (Task.Delay) so fast typing doesn't start a search per key
        Sleep(200);
        if (*cancel)
            return;
        struct Found
        {
            std::mutex lock;
            std::vector<std::wstring> paths;
        } found;
        auto sink = [](void* context, const uint16_t* path, size_t len)
        {
            auto* f = static_cast<Found*>(context);
            std::lock_guard guard(f->lock);
            if (f->paths.size() < kMax)
                f->paths.emplace_back(reinterpret_cast<const wchar_t*>(path), len);
        };
        FfSearchProgress progress{};
        static_assert(sizeof(bool) == 1);
        bool stop = false;
        std::thread watcher([&] { while (!stop) { if (*cancel) stop = true; Sleep(30); } });
        ff_search(reinterpret_cast<const uint16_t*>(folder.c_str()), folder.size(), reinterpret_cast<const uint16_t*>(text.c_str()),
            text.size(), hidden ? 1u : 0u, kMax, 2, &progress, &stop, sink, &found);
        stop = true;
        watcher.join();
        if (*cancel)
            return;
        auto* entries = new std::vector<Entry>;
        for (const auto& p : found.paths)
        {
            Entry e;
            e.kind = Kind::Item;
            e.path = p;
            e.text = PathFindFileNameW(p.c_str());
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON))
                e.icon = sfi.iIcon;
            entries->push_back(std::move(e));
        }
        if (!PostMessageW(owner, kMsgItems, request, reinterpret_cast<LPARAM>(entries)))
            delete entries;
    }).detach();
}

LRESULT CALLBACK SearchSuggest::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
    {
        auto* created = static_cast<SearchSuggest*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        created->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<SearchSuggest*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SearchSuggest::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
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
            PostMessageW(m_owner, kMsgPick, static_cast<WPARAM>(row), 0);
        return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}
