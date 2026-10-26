#pragma once
#include "Theme.h"
#include "SmoothScroll.h"

// Files' command palette lives in the omnibar: type ">" (or press Ctrl+Shift+P) and the address box filters
// commands. This is the list under it; the address edit keeps the focus, the list never activates.
class CommandPalette
{
public:
    struct Entry
    {
        int id = 0;
        std::wstring name;
        std::wstring category;
        std::wstring keys; // effective shortcut text, may be empty
    };

    ~CommandPalette();
    bool Create(HWND owner, const Theme* theme);
    void SetEntries(std::vector<Entry> entries) { m_all = std::move(entries); }
    void Show(const RECT& below); // screen rect of the omnibar
    void Hide();
    bool Visible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    void Filter(const std::wstring& text);
    void Move(int delta);
    int Selected() const; // command id, 0 if none
    void Relayout();

    // Posted to the owner when an entry is clicked: wParam = command id
    static constexpr UINT kMsgRun = WM_APP + 70;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);
    void Resize();
    int RowAt(int y) const;
    void EnsureVisible();

    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    const Theme* m_theme = nullptr;
    std::vector<Entry> m_all;
    std::vector<int> m_shown; // indices into m_all
    int m_selected = 0;
    int m_hot = -1;
    int m_scroll = 0;         // first visible row (SmoothScroll::Value: fractional rows while animating)
    SmoothScroll m_smooth;    // the wheel's animation (Move / Filter jump straight there: m_smooth.Reset)
    RECT m_below{};
};
