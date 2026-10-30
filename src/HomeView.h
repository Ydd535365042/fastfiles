#pragma once
#include "Theme.h"
#include "SmoothScroll.h"
#include "ScrollOverlay.h"

// One card or row on the home page. Built by MainWindow from the sidebar's entries and the recent-files scan.
struct HomeItem
{
    UniquePidl pidl;       // null for lazy entries (UNC pins) and recent files (opened by path)
    std::wstring name;
    std::wstring path;     // parsing name / file path; also the pin key
    std::wstring link;     // recent files: the .lnk in the Recent folder (deleted by "remove from recent")
    int icon = -1;         // system image list index
    bool missing = false;
    ULONGLONG freeBytes = 0, totalBytes = 0;
    FILETIME time{};       // recent files: last opened
    bool folder = false;   // tagged items: folders open in place, files launch
};

// Files' FileTagsWidget: one container per tag (WidgetFileTagsContainerItem) with its items
struct HomeTagGroup
{
    std::wstring id, name;
    COLORREF color = 0;
    std::vector<HomeItem> items;
};

struct HomeData
{
    std::vector<HomeItem> quick, drives, network, recent;
    std::vector<HomeTagGroup> tags;
    bool recentReady = false; // false until the first scan finishes (shows "loading" instead of "none")
    bool tagsReady = false;
};

// Worker-thread scan of %APPDATA%\Microsoft\Windows\Recent (newest first, files only, no network access)
std::vector<HomeItem> ScanRecentFiles(size_t max);
// Worker thread: every tag with its items (missing local ones left out), at most maxPerTag each
std::vector<HomeTagGroup> ScanTaggedItems(size_t maxPerTag);
HomeTagGroup CopyTagGroup(const HomeTagGroup& group);
HomeItem CopyHomeItem(const HomeItem& item);

// OpenTag / OpenAllTagged: key is the tag id (Files' ViewMoreCommand / OpenAllCommand)
enum class HomeAction { Pin, Unpin, RemoveRecent, ClearRecent, Refresh, ToggleSection, OpenTag, OpenAllTagged };

struct HomeEvents
{
    virtual void OnHomeOpen(PCIDLIST_ABSOLUTE pidl, OpenMode mode) = 0;
    virtual void OnHomeReveal(PCIDLIST_ABSOLUTE item) = 0; // open the containing folder with the item selected
    virtual void OnHomeAction(HomeAction action, const std::wstring& key, PCIDLIST_ABSOLUTE pidl, int section) = 0;
};

// Files' home page (Views/HomePage.xaml + Widgets), drawn like design/ui-blueprint.html region 8:
// Quick access cards, drive cards with capacity bars, network locations, recent files.
class HomeView
{
public:
    // Bit order (collapsed/hidden masks, sessions) - not display order: tags show between network and recent, as in Files
    enum Section : int { SecQuick, SecDrives, SecNetwork, SecRecent, SecTags, SecCount };

    explicit HomeView(HomeEvents* events) : m_events(events) {}
    ~HomeView();

    bool Create(HWND parent, const Theme* theme);
    void Destroy();
    HWND Hwnd() const { return m_hwnd; }
    void FocusByKeyboard(); // Tab arrived: the focus visual on the stop that has it (the first one if none yet)
    void SetData(std::shared_ptr<const HomeData> data, unsigned collapsed, unsigned hidden); // Section bits
    void Relayout();

private:
    // item: >= 0 an item; -1 section header; -2 empty-section hint; -3 tag container (backdrop only);
    // -4 tag title (opens the tag); -5 the container's "..." button. group: the tag (SecTags only)
    enum : int { kHeader = -1, kHint = -2, kContainer = -3, kTagTitle = -4, kTagMore = -5 };
    struct Cell
    {
        Section section = SecQuick;
        int item = -1;
        RECT rc{};     // content coordinates
        int group = -1;
    };
    static bool Interactive(int item) { return item != kHint && item != kContainer; }
    const HomeTagGroup* GroupAt(int cell) const;
    void ShowTagMenu(int cell, POINT screen);
    struct Resolved; // worker -> UI: lazy path parsed into a PIDL
    static constexpr UINT kMsgResolved = WM_APP + 40;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    static const wchar_t* SectionName(int section);
    const std::vector<HomeItem>* Items(Section section) const;
    const HomeItem* ItemAt(int cell) const;

    void Layout();
    void Paint(HDC hdc, const RECT& rc);
    void PaintCard(HDC hdc, const Cell& cell, const HomeItem& item, RECT r, bool hot, bool down);
    void PaintRecent(HDC hdc, const HomeItem& item, RECT r, bool hot, bool down);
    void PaintTagCell(HDC hdc, const Cell& cell, RECT r, bool hot, bool down);
    void DrawIcon(HDC hdc, int index, int x, int y, int size);
    int CellAt(POINT client) const;
    RECT ClientRect(int cell) const;
    void SetHot(int cell);
    void ScrollTo(int offset);
    void EnsureVisible(int cell);
    void MoveFocus(int key);
    void Activate(int cell, OpenMode mode);
    void OpenLazy(const std::wstring& path, OpenMode mode, bool reveal = false);
    void ShowMenu(int cell, POINT screen);
    std::wstring TipFor(int cell) const;

    HomeEvents* m_events;
    const Theme* m_theme = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_tooltip = nullptr;
    HFONT m_title = nullptr; // semibold section titles
    std::shared_ptr<const HomeData> m_data;
    unsigned m_collapsed = 0;
    unsigned m_hidden = 0; // widgets turned off in settings
    std::vector<Cell> m_cells;
    int m_content = 0; // content height
    int m_scroll = 0;
    SmoothScroll m_smooth; // the wheel's animation (the scrollbar thumb drag stays immediate)
    ScrollOverlay m_bar;   // the ScrollViewer's vertical bar (the same WinUI ScrollBar as the file list's)
    static constexpr UINT_PTR kBarTimer = 1;
    int m_hot = -1, m_pressed = -1, m_focus = -1;
    bool m_keyboardFocus = false;
    bool m_tracking = false;
    int m_tipCell = -1;
    std::wstring m_tipText;
    std::vector<std::pair<long long, HICON>> m_iconCache; // (size << 32 | index) -> icon
};
