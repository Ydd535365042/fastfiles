#pragma once
#include "Common.h"

// Files' wheel scroll (WinUI ScrollViewer / ListView): each notch animates the offset with a ~250 ms cubic
// ease-out (Theme::AnimProgress's curve); a second notch before the first settles retargets the same animation
// so the velocity stays continuous (no queued jumps). A touchpad's high-resolution WM_MOUSEWHEEL deltas (|delta|
// smaller than WHEEL_DELTA, or not a whole multiple of it) follow the finger 1:1 with no added smoothing lag, the
// same as Files. "Show animations" off (SPI_GETCLIENTAREAANIMATION, Theme::AnimationsEnabled) jumps immediately.
//
// One instance per scrollable axis (FileList's m_scroll.y, Sidebar's m_scroll, ...). Call OnWheel from the
// WM_MOUSEWHEEL handler exactly where the old `clamp(pos - MulDiv(delta, step, WHEEL_DELTA), 0, max)` line was;
// it returns the value to assign for this frame and keeps the window posted for the frames after it (SmoothFrame
// below), paced to the monitor's refresh rate instead of a 15 ms SetTimer, until the animation settles.
class SmoothScroll
{
public:
    ~SmoothScroll();
    SmoothScroll() = default;
    SmoothScroll(const SmoothScroll&) = delete;
    SmoothScroll& operator=(const SmoothScroll&) = delete;

    // A wheel notch (or touchpad delta) arrived at `hwnd`'s WM_MOUSEWHEEL: moves the target by
    // -delta * step / WHEEL_DELTA, clamped to [0, maxValue], and (re)starts the animation toward it (or jumps
    // immediately: a high-resolution delta, or animations off). Returns this frame's value.
    int OnWheel(HWND hwnd, int delta, int step, int maxValue);
    void Reset(int value);          // a relayout under the cursor: snap with no animation, stop ticking
    void ClampTarget(int maxValue); // content got shorter: pull current/target back in range, keep animating if it was
    int Value() const { return static_cast<int>(m_current + 0.5); }
    bool Animating() const { return m_animating; }

    // One frame toward the target (SmoothFrame's tick calls this). Returns the new value; stops and unregisters
    // itself once the target is reached.
    int Tick();

private:
    void StartOrRetarget(HWND hwnd, int newTarget, bool immediate);
    // The curve of the running animation at `now` (ms, NowMs): position and velocity (px per ms). A cubic from
    // (m_from, m_v0) to (m_target, 0) over kDurationMs: a fresh start (m_v0 = 3 * distance / duration) is exactly the
    // cubic ease-out; a retarget starts from the current position and velocity, so no frame stalls or jumps.
    double At(double now, double* velocity = nullptr) const;
    static double NowMs(); // QueryPerformanceCounter (GetTickCount64's 15.6 ms steps made uneven frames)
    double m_current = 0, m_from = 0, m_v0 = 0;
    int m_target = 0;
    double m_start = 0;
    static constexpr int kDurationMs = 250; // Files' ScrollViewer wheel animation
    bool m_animating = false;
    HWND m_hwnd = nullptr;
};

// The frame clock: paces ticks to the display's refresh rate (DwmGetCompositionTimingInfo, EnumDisplaySettings
// fallback) instead of SetTimer's ~15.6 ms granularity, per monitor, updated on WM_DISPLAYCHANGE / a window
// moving to another monitor. A worker thread waits for vblank (DwmFlush, or a high-resolution waitable timer on
// Windows 8 where DwmFlush isn't paced to a specific monitor) and posts one coalesced kMsgTick per registered
// window per frame; it parks with no wake-ups while nothing is animating.
namespace SmoothFrame
{
constexpr UINT kMsgTick = WM_APP + 90; // posted to a registered window once per frame while it's animating

// Keeps `hwnd` ticking (SmoothScroll::Tick on its scrollers, from its WM_APP+90 handler) until it stops
// registering (an animation settles without re-registering, or Forget() tears it down with the window).
void Register(HWND hwnd);
void Forget(HWND hwnd); // the window is being destroyed: drop it even mid-animation

// The refresh rate SmoothFrame is pacing `hwnd`'s monitor to (for tests / diagnostics); 0 before it's measured.
double RefreshRateFor(HWND hwnd);

// WM_DISPLAYCHANGE: a monitor's own rate may have changed without any window moving to a different HMONITOR: the
// next frame re-measures regardless. Cheap and safe to call even when nothing is animating.
void NotifyDisplayChange();

// Test diagnostics only (Sandbox::Active()): how many frames WorkerMain has posted out in total, process-wide,
// since startup. A test polls this (SendMessageW the registered "FastFiles.DebugFrameTick" message, any window)
// at high frequency and counts how it changes over a wall-clock window to measure the actual delivered fps,
// instead of guessing from the animation's nominal duration.
unsigned DebugFrameCount();
}
