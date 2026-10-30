#pragma once
#include "Theme.h"

struct ButtonBarEvents
{
    // Plain buttons arrive as WM_COMMAND/BN_CLICKED on the parent; dropdowns call this with the button's screen rect
    virtual void OnBarDropdown(int id, const RECT& screen) = 0;
    virtual void OnBarContext(int, const RECT&) {}
};

// Flat Fluent-style button row: glyph + optional label, rounded hover/pressed/checked backgrounds.
// id 0 is a separator, id -1 a spacer that takes the remaining width (pushes later items right).
class ButtonBar
{
public:
    struct Item
    {
        int id = 0;
        const wchar_t* glyph = nullptr;
        std::wstring label;
        std::wstring tip;
        bool dropdown = false;
        bool enabled = true;
        bool checked = false;
        bool visible = true;
        RECT rc{};
    };

    // background points into the theme so a palette switch repaints with the new color
    bool Create(HWND parent, int id, const Theme* theme, ButtonBarEvents* events, std::vector<Item> items, const COLORREF* background);
    HWND Hwnd() const { return m_hwnd; }
    void SetEnabled(int id, bool enabled);
    void SetChecked(int id, bool checked);
    void SetGlyph(int id, const wchar_t* glyph);
    void SetVisible(int id, bool visible);
    void SetItems(std::vector<Item> items); // replace everything (customized toolbar), tooltips included
    void SetSidebarLook(bool on) { m_sidebarLook = on; } // SidebarItem states (the sidebar footer's 设置), see Paint
    void SetButtonLook(bool on) { m_buttonLook = on; }   // DefaultButtonStyle states (the nav row's buttons), see Paint
    int ItemAt(POINT client) const;         // id of the button there, 0 for none
    bool IsEnabled(int id) const;
    RECT ScreenRect(int id) const;
    int IdealWidth();
    void Relayout();
    // Keyboard focus (the window's Tab order): the buttons Tab stops at, in order, and the one that has it. Each
    // button is its own stop, as WinUI buttons are; Enter / Space presses it. The ring shows only after keyboard focus.
    std::vector<int> FocusableIds() const;
    void FocusItem(int id); // takes the focus with the ring on that button
    int FocusedId() const;  // 0 when the bar doesn't have the focus

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC hdc, const RECT& rc);
    int HitTest(POINT pt) const;
    int ItemWidth(HDC hdc, const Item& item, bool compact) const;
    std::vector<bool> Shown() const; // visible, minus separators left dangling by hidden buttons
    Item* Find(int id);
    void Fire(int index);

    HWND m_hwnd = nullptr;
    HWND m_tooltip = nullptr;
    const Theme* m_theme = nullptr;
    ButtonBarEvents* m_events = nullptr;
    std::vector<Item> m_items;
    const COLORREF* m_background = nullptr;
    int m_hot = -1;
    int m_pressed = -1;
    int m_flash = -1;          // a quick click's button, still drawn pressed until kFlashTimer
    ULONGLONG m_pressedAt = 0;
    static constexpr UINT_PTR kFlashTimer = 1;
    static constexpr ULONGLONG kFlashMs = 120;
    std::vector<StateFade> m_fades; // per item: the hover / pressed fill easing between states
    static constexpr UINT_PTR kFadeTimer = 2;
    bool m_tracking = false;
    bool m_sidebarLook = false;
    bool m_buttonLook = false;
    int m_focus = -1;    // item index with the keyboard focus (-1 none)
    bool m_ring = false; // the focus came from the keyboard (a press with the mouse hides the ring again)
};

// Single-line status area: "left\tright" puts the part after the tab on the right edge. An optional Git branch chip
// sits at the far right (Files' status bar); clicking it sends WM_COMMAND(id, kBranchClick) to the parent.
class StatusBar
{
public:
    static constexpr int kBranchClick = 1;
    static constexpr int kSyncClick = 3;
    bool Create(HWND parent, int id, const Theme* theme);
    HWND Hwnd() const { return m_hwnd; }
    void SetBranch(const std::wstring& branch); // empty hides the chip
    RECT BranchScreenRect() const { return ChipScreenRect(kBranch); }
    // Files' GitNetworkActions: "outgoing / incoming" commits, left of the branch; empty text hides it
    void SetSync(const std::wstring& text, const std::wstring& tip);
    RECT SyncScreenRect() const { return ChipScreenRect(kSync); }
    // Keyboard (Files' GitNetworkActions / GitBranch are Buttons: each its own Tab stop, Enter / Space press them)
    std::vector<int> FocusableIds() const; // the shown chips, left to right
    void FocusItem(int chip);
    int FocusedId() const;                 // -1 when the bar hasn't the focus

private:
    enum { kSync, kBranch, kChips };
    struct Chip
    {
        std::wstring text;
        std::wstring tip;
        const wchar_t* glyph = nullptr;
        RECT rc{};
    };
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC hdc, RECT rc);
    int HitChip(POINT pt) const;
    RECT ChipScreenRect(int chip) const;
    HWND m_hwnd = nullptr;
    const Theme* m_theme = nullptr;
    Chip m_chips[kChips];
    int m_hot = -1;
    bool m_tracking = false;
    int m_focus = -1;
    bool m_ring = false;      // the focus visual: after keyboard focus only (a pointer press hides it)
    HWND m_tooltip = nullptr; // one tool per chip; text from Chip::tip
};
