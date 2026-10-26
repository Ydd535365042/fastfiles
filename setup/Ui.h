#pragma once
#include "Setup.h"
#include "Draw.h"
#include "../src/RoundFrame.h"
#include <thread>

// The setup wizard: one borderless window with rounded corners (Windows 11: DWM's; Windows 8 / 10: RoundFrame's
// region, border and shadow), everything self-drawn.
// Pages: Ready (welcome / confirm uninstall) -> Working (progress) -> Done or Failed. Pages slide and fade into each other.
class Ui
{
public:
    explicit Ui(Options& o) : m_o(o) {}
    int Run();

private:
    enum class Page { Splash, Ready, Working, Done, Failed };
    enum Ctl : int { CtlNone = -1, CtlMin, CtlClose, CtlBrowse, CtlCheck, CtlPrimary, CtlSecondary, CtlCount };
    struct Anim { double value = 0, target = 0; }; // hover/press fades, 0..1
    // 96-DPI layout: window, title bar, command area (WinUI ContentDialog proportions); page slide duration
    static constexpr int kW = 600, kH = 400, kTitle = 32, kFooter = 80, kSlideMs = 260;
    static constexpr int kSplashMs = 900; // launch animation: icon + name, then flows into Ready

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    int S(int v) const { return MulDiv(v, static_cast<int>(m_dpi), 96); }

    // Layout and state
    void Layout();
    bool Visible(int ctl) const;
    bool Enabled(int ctl) const;
    int HitTest(POINT pt) const;
    void Activate(int ctl);
    void Go(Page page);
    void Start();
    void Browse();
    void Finish(int code);
    void Animate(); // one frame of every running animation
    void Kick();    // make sure the frame timer runs
    void SetHot(int ctl);
    void Theme();

    // Painting (UiPaint.cpp)
    void Paint(HDC hdc);
    void PaintTitle(HDC hdc);
    void PaintPage(HDC hdc, Page page);
    void PaintButton(HDC hdc, int ctl, const std::wstring& text, bool accent);
    void PaintCheck(HDC hdc, const std::wstring& text, bool on);
    void PaintBadge(HDC hdc, int cx, int cy, bool ok);
    void PaintRing(HDC hdc, int cx, int cy, double radius); // Working page: indeterminate progress ring
    void Snapshot();
    std::wstring PrimaryText() const;
    bool Checked() const { return m_o.uninstall ? m_o.deleteData : m_page == Page::Done ? m_o.launch : m_o.desktopIcon; }

    Options& m_o;
    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    Palette m_p = MakePalette(false);
    Fonts m_f;
    RoundFrame m_round;
    Page m_page = Page::Ready;
    RECT m_rc[CtlCount]{};
    Anim m_hover[CtlCount]{};
    Anim m_checkFill;
    int m_hot = CtlNone, m_pressed = CtlNone, m_focus = CtlNone;
    bool m_keyboard = false, m_tracking = false, m_animating = false;
    bool m_animations = true; // SPI_GETCLIENTAREAANIMATION
    std::wstring m_previous;  // installed version (update instead of install)

    // Transition: the old page as a bitmap, the new one drawn live over it
    HBITMAP m_old = nullptr;
    ULONGLONG m_pageStart = 0;
    // Worker
    std::thread m_worker;
    int m_target = 0;     // percent reported
    double m_shown = 0;   // percent drawn (eases toward m_target)
    std::wstring m_status, m_error;
    std::wstring m_statusFrom;     // status cross-fade: previous text, fading out while m_status fades in
    ULONGLONG m_statusStart = 0;
    bool m_finished = false, m_ok = false;
    int m_exit = ExitCancelled;
};
