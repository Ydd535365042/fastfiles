#pragma once
#include "Theme.h"
#include "SmoothScroll.h"
#include "ScrollOverlay.h"
#include <functional>

// One line on a settings page. Values are read through the callbacks every paint, so the page always shows
// what is actually in effect (shell settings shared with Explorer can change behind our back).
struct SettingsRow
{
    // Colors: Files' background color SettingsExpander (color button in the header, preview cards below; uses
    // options / selected / select for the cards, press for the color button)
    enum class Kind { Header, Note, Toggle, Choice, Button, Value, Shortcut, Colors };
    Kind kind = Kind::Note;
    std::wstring label;
    std::wstring detail; // explanation: an (i) after the label opens it in a bubble
    const wchar_t* icon = nullptr;        // SettingsCard HeaderIcon glyph
    std::vector<uint32_t> colors;         // Colors: #AARRGGBB per card
    std::function<uint32_t()> color;      // Colors: the color the button shows

    std::function<bool()> isOn;           // Toggle
    std::function<void(bool)> setOn;
    std::vector<std::wstring> options;    // Choice
    std::function<int()> selected;
    std::function<void(int)> select;
    std::wstring button;                  // Button / Value: action label (Value may have none)
    std::function<void()> press;
    std::function<std::wstring()> value;  // Value: text shown on the right

    std::wstring command;                 // Shortcut: command key
    std::function<std::vector<uint32_t>()> chords;
    std::function<bool()> overridden;     // shows "恢复默认"

    bool swatch = false;                  // colored dot before the label (tags)
    COLORREF swatchColor = 0;
};

struct SettingsPage
{
    const wchar_t* glyph = nullptr;
    std::wstring name;
    std::vector<SettingsRow> rows;
};

struct SettingsHost
{
    virtual std::vector<SettingsPage> BuildSettingsPages() = 0;
    // Returns an error to show on the row, or empty when the shortcut was taken
    virtual std::wstring SetShortcut(const std::wstring& command, uint32_t code) = 0;
    virtual void ResetShortcut(const std::wstring& command) = 0;
    virtual void ClearShortcut(const std::wstring& command) = 0;
};

// Files' SettingsPage (Views/Settings/SettingsPage.xaml): shown in a tab like Home, a 240px navigation pane with the
// "设置" header on the left, the page's SettingsCards in a scroll viewer on the right.
class SettingsView
{
public:
    explicit SettingsView(SettingsHost* host) : m_host(host) {}
    ~SettingsView();

    bool Create(HWND parent, const Theme* theme, int page); // child window, hidden until the tab shows it
    HWND Hwnd() const { return m_hwnd; }
    // Tab arrived: forward the navigation pane (Files' SidebarView: one stop, the arrows move, Enter opens), Shift+Tab
    // the last card. TabWithin: navigation -> the cards -> out; false when Tab has run past the page's first / last stop.
    void FocusByKeyboard(bool back);
    bool TabWithin(bool back); // Tab / Shift+Tab inside the page: false at its end
    int Page() const { return m_page; }
    bool Capturing() const { return m_capture >= 0; } // recording a shortcut: every key is the page's
    // Keys the page takes before the app's shortcuts: all of them while a dropdown is open, F4 on a ComboBox
    bool WantsKey(UINT vk) const;
    void ShowPage(int page);
    void Rebuild();  // values or row set changed (keeps the page and scroll position)
    void Relayout(); // theme / DPI change

private:
    struct Cell
    {
        int row = -1;
        RECT rc{};        // content coordinates
        RECT control{};   // toggle / dropdown / button / chord chips
        RECT reset{};     // shortcut: restore default
        RECT clear{};     // shortcut: remove
        RECT info{};      // (i) after the label (rows with a detail)
        std::vector<RECT> tiles; // Colors: the preview cards
    };
    enum class Part { None, Row, Control, Reset, Clear, Info, Tile };

    // Files' ComboBox dropdown (WinUI ComboBox popup): drawn over the page, the selected item over the box
    struct Dropdown
    {
        int cell = -1;     // open for this cell, -1 = closed
        RECT box{};        // client coordinates
        int itemH = 0;
        int visible = 0;   // whole rows that fit
        int scroll = 0;    // pixels
        int hot = -1;
        int pressed = -1;
        int current = -1;
        std::wstring typed; // type-ahead
        ULONGLONG typedAt = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Layout();
    void Paint(HDC hdc, const RECT& rc);
    void PaintRow(HDC hdc, const Cell& cell, int offsetY);
    RECT NavRect(int page) const;
    RECT ContentClient() const;
    int NavAt(POINT pt) const;
    int CellAt(POINT pt, Part* part, int* tile = nullptr) const;
    bool Focusable(int cell) const;
    void MoveFocus(int delta);
    void EnsureVisible(int cell);
    void ScrollTo(int offset);
    void Activate(int cell, Part part, int tile = -1);
    void OpenDropdown(int cell);
    void CloseDropdown();
    void CommitDropdown(int item);
    int DropdownItemAt(POINT pt) const;
    void DropdownScrollTo(int offset);
    void DropdownMoveHot(int item);
    bool DropdownKey(UINT vk);
    void DropdownChar(wchar_t ch);
    void PaintDropdown(HDC hdc);
    void PaintColors(HDC hdc, const Cell& cell, int offsetY);
    void PublishDropdown(); // window text of the view lists the open dropdown (tests)
    void BeginCapture(int cell);
    void EndCapture();
    void OnCaptureKey(UINT vk);
    void SetBubble(int cell); // the explanation bubble of this cell's (i), -1 = none
    void PaintBubble(HDC hdc, int offsetY);
    void PaintInfo(HDC hdc, const Cell& cell, int offsetY);

    SettingsHost* m_host;
    const Theme* m_theme = nullptr;
    HWND m_hwnd = nullptr;
    HFONT m_titleFont = nullptr;
    HFONT m_headerFont = nullptr;
    HFONT m_cardIcons = nullptr;   // SettingsCard HeaderIcon: 20px
    Dropdown m_drop;
    bool m_colorsCollapsed = false; // Files' background color expander is open by default
    int m_hotTile = -1;
    int m_pressedTile = -1;
    std::vector<SettingsPage> m_pages;
    int m_page = 0;
    std::vector<Cell> m_cells;
    int m_content = 0;
    int m_scroll = 0;
    SmoothScroll m_smooth; // the page body's wheel animation (the dropdown popup's own scroll stays immediate)
    int m_navHot = -1;
    bool m_navFocus = false; // the keyboard is on the navigation pane (m_navKey), not on a card
    int m_navKey = 0;        // the navigation item with the keyboard (the arrows move it, Enter opens its page)
    void MoveNav(int to);
    int m_hot = -1;
    Part m_hotPart = Part::None;
    int m_pressed = -1;
    Part m_pressedPart = Part::None;
    int m_focus = -1;
    bool m_keyboard = false;
    bool m_tracking = false;
    ScrollOverlay m_bar;           // the content ScrollViewer's vertical bar (the same WinUI ScrollBar as the file list's)
    int m_capture = -1;            // cell waiting for a key press
    int m_bubble = -1;             // cell whose explanation bubble is open
    std::wstring m_messageCommand; // shortcut row showing m_message (survives Rebuild)
    std::wstring m_message;

    // Animations (skipped when Windows' animations are off): toggle knob slide, page entrance, nav indicator slide
    void Animate();           // keep the frame timer running while something moves
    bool Animating() const;
    int m_toggleRow = -1;     // row index (on m_page) whose knob is sliding
    ULONGLONG m_toggleStart = 0;
    ULONGLONG m_pageStart = 0;
    ULONGLONG m_navStart = 0;
    int m_navFrom = -1;       // page the indicator slides from
};
