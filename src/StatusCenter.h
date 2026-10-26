#pragma once
#include "Operations.h"
#include "Theme.h"
#include "SmoothScroll.h"
#include <map>

// Files' status center: the address bar button (StatusCenter icon, or a progress ring with a count badge while
// operations run, colored by InfoBadgeState) and its flyout of cards (StatusCenter.xaml: header, clear completed,
// one card per operation with progress bar / speed graph / cancel). Also the taskbar progress and the one-time teaching
// tip. Polls Operations on its own timer while anything runs.
class StatusCenter
{
public:
    // WM_COMMAND codes sent to the parent (LOWORD = the button's id)
    static constexpr int kVisibilityChanged = 1; // Wanted() changed: lay the address bar out again
    static constexpr int kTipShown = 2;          // the teaching tip was shown (save ShowStatusCenterTeachingTip = false)
    static constexpr int kClearCompleted = 0xC1; // WM_COMMAND to the flyout: Files' 清除已完成的条目 (tests)
    static constexpr int kWindowInactive = 3;    // the flyout lost the activation to another window than the parent

    ~StatusCenter();
    bool Create(HWND parent, int id, const Theme* theme, Operations* ops);
    HWND Button() const { return m_button; }
    HWND Flyout() const { return m_flyout; }
    void SetAlways(bool always) { m_always = always; }        // Files' StatusCenterVisibility.Always
    void SetTipPending(bool pending) { m_tipPending = pending; } // Files' ShowStatusCenterTeachingTip
    bool Wanted() const;  // the button shows (setting, or operations listed)
    void Refresh();       // operations changed (also the timer): cards, speed, ring, taskbar
    void Toggle();        // the button: open / close the flyout
    bool Open() const { return m_flyout && IsWindowVisible(m_flyout); }
    // Files' flyout is part of the window, which stays active under it: the window keeps its active look (tab row,
    // border) while this one has the activation, instead of turning inactive and back on every open and close
    bool KeepsWindowActive() const { return m_showing || Open(); }
    void Hide();
    void Relayout();      // DPI / fonts / theme

    // Files' StatusCenterHelper texts (also the window's status line after an operation finishes)
    static std::wstring Header(const Operations::View& v);
    static std::wstring Message(const Operations::View& v);

private:
    struct Track // per card, the UI side (Files' StatusCenterItem speed / graph / IsExpanded)
    {
        bool expanded = false;
        ULONGLONG at = 0, bytes = 0, files = 0; // last sample
        double speed = 0;                      // bytes/s, or items/s without a total size
        bool bySize = true;
        std::vector<std::pair<float, float>> points; // percent, speed
    };
    struct Layout // one card's parts, relative to the card's top-left
    {
        int height = 0, headerLines = 1;
        RECT circle{}, header{}, dismiss{}, expand{}, bar{}, message{}, more{}, graph{}, name{};
        bool progress = false, graphShown = false;
    };
    static LRESULT CALLBACK ButtonProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FlyoutProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT ButtonMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT FlyoutMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void PaintButton(HDC hdc, const RECT& rc);
    void PaintFlyout(HDC hdc, const RECT& rc);
    void PaintCard(HDC hdc, const Operations::View& v, const RECT& card, const Layout& l);
    Layout LayoutCard(const Operations::View& v, int width) const;
    int ContentHeight() const;
    void Resize();
    void UpdateText();    // the flyout's window text: one line per card (tests read it)
    void UpdateTaskbar();
    void ShowTip();
    void HideTip();
    int InfoState() const; // Files' InfoBadgeState: 0 all done, 1 running, 2 running with an error, 3 done with an error
    int Running() const;
    int Average() const;   // Files' AverageOperationProgressValue
    enum Hit { HitNone, HitClear, HitDismiss, HitExpand, HitMore };
    Hit HitTest(POINT pt, unsigned& id) const;
    RECT ClearRect() const;
    RECT ListRect() const;

    HWND m_parent = nullptr, m_button = nullptr, m_flyout = nullptr, m_tip = nullptr, m_cardTip = nullptr;
    int m_id = 0;
    const Theme* m_theme = nullptr;
    Operations* m_ops = nullptr;
    HFONT m_title = nullptr; // BodyStrongTextBlockStyle, 14px
    std::vector<Operations::View> m_views; // newest first
    std::map<unsigned, Track> m_tracks;
    bool m_always = true, m_tipPending = false, m_wanted = false;
    bool m_ring = false;        // Files' ShowProgressRing: on while something runs, off once the flyout was opened
    bool m_hot = false, m_down = false, m_tracking = false, m_flyTracking = false;
    int m_hotHit = HitNone;
    unsigned m_hotId = 0, m_tipId = 0;
    int m_scroll = 0;
    SmoothScroll m_smooth; // the flyout's wheel animation
    ULONGLONG m_hiddenAt = 0;   // the flyout closed (a click on the button that closed it doesn't reopen it)
    SIZE m_region{};            // the flyout's size its rounded region was made for
    bool m_showing = false;     // Toggle is showing the flyout (the window must keep its active look meanwhile)
    ComPtr<ITaskbarList3> m_taskbar;
    int m_taskbarState = -1, m_taskbarValue = -1;
    std::wstring m_text, m_cardTipText;
};
