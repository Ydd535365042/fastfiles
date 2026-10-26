#include "SmoothScroll.h"
#include "Theme.h"
#include <dwmapi.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <atomic>

// --- SmoothScroll: one scroll axis's animation state ---

SmoothScroll::~SmoothScroll()
{
    if (m_hwnd)
        SmoothFrame::Forget(m_hwnd); // never leaves a stale pointer registered after the view is destroyed
}

void SmoothScroll::StartOrRetarget(HWND hwnd, int newTarget, bool immediate)
{
    m_hwnd = hwnd;
    m_target = newTarget;
    if (immediate || !AnimationsEnabled())
    {
        m_current = m_from = newTarget;
        m_animating = false;
        return;
    }
    // Retargeting keeps the velocity continuous: the animation restarts from wherever it is now (m_current), not
    // from the old target, so a rapid run of notches doesn't jerk back to a stale start point (Files/WinUI: the
    // ScrollViewer's existing DoubleAnimation is retargeted the same way).
    m_from = m_current;
    m_start = GetTickCount64();
    m_animating = true;
    SmoothFrame::Register(hwnd);
}

int SmoothScroll::OnWheel(HWND hwnd, int delta, int step, int maxValue)
{
    maxValue = std::max(0, maxValue);
    // A touchpad's high-resolution WM_MOUSEWHEEL: a delta smaller than one notch, or one that isn't a whole
    // multiple of WHEEL_DELTA, follows the finger directly (Files: ScrollViewer under PointerWheelChanged isn't
    // involved at all for precision touchpad input, which comes as direct manipulation instead). A multi-notch
    // delta from an ordinary wheel (240, 360, ...: some mice/drivers coalesce several ticks into one message)
    // is still a whole multiple of WHEEL_DELTA and animates, same as a single notch.
    const bool precise = delta != 0 && (std::abs(delta) < WHEEL_DELTA || (std::abs(delta) % WHEEL_DELTA) != 0);
    const int target = std::clamp(m_animating ? m_target : Value(), 0, maxValue);
    const int by = -MulDiv(delta, step, WHEEL_DELTA);
    const int newTarget = std::clamp(target + by, 0, maxValue);
    StartOrRetarget(hwnd, newTarget, precise);
    return Value();
}

void SmoothScroll::Reset(int value)
{
    m_current = m_from = m_target = value;
    m_animating = false;
    if (m_hwnd)
        SmoothFrame::Forget(m_hwnd);
}

void SmoothScroll::ClampTarget(int maxValue)
{
    maxValue = std::max(0, maxValue);
    const int clamped = std::clamp(m_target, 0, maxValue);
    if (clamped == m_target && Value() <= maxValue)
        return;
    m_target = clamped;
    if (m_current > maxValue)
        m_current = m_from = maxValue;
    if (!m_animating)
        m_current = m_from = m_target;
}

int SmoothScroll::Tick()
{
    if (!m_animating)
        return Value();
    const double p = AnimProgress(m_start, kDurationMs);
    m_current = m_from + (m_target - m_from) * p;
    if (p >= 1.0)
    {
        m_current = m_target;
        m_animating = false;
        if (m_hwnd)
            SmoothFrame::Forget(m_hwnd);
    }
    else if (m_hwnd)
        SmoothFrame::Register(m_hwnd); // still easing: back into WorkerMain's `active` set for the next frame too
    return Value();
}

// --- SmoothFrame: the shared, refresh-rate-paced clock every SmoothScroll animation ticks on ---

namespace SmoothFrame
{
namespace
{
template <class F> F Lookup(const wchar_t* dll, const char* name)
{
    static HMODULE h = LoadLibraryW(dll); // kernel32 / dwmapi are already loaded; this just finds the handle
    return h ? reinterpret_cast<F>(GetProcAddress(h, name)) : nullptr;
}

// CreateWaitableTimerExW's CREATE_WAITABLE_TIMER_HIGH_RESOLUTION flag is Windows 10 1803+; looked up at run time
// (Compat.cpp's pattern) so the import table itself only ever needs CreateWaitableTimerExW, which exists since
// Windows 8 (without the high-resolution flag, it still works: just the ~15.6 ms system tick, same as before).
constexpr DWORD kHighResTimerFlag = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION

HANDLE CreateFrameTimer()
{
    using CreateFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
    static const auto create = Lookup<CreateFn>(L"kernel32.dll", "CreateWaitableTimerExW");
    if (create)
        if (HANDLE h = create(nullptr, nullptr, kHighResTimerFlag, TIMER_ALL_ACCESS))
            return h;
    return CreateWaitableTimerW(nullptr, FALSE, nullptr); // Windows 8 (or the high-res flag refused): ~15.6 ms ticks
}

double MonitorRefreshHz(HMONITOR monitor)
{
    // DwmGetCompositionTimingInfo exists since Windows 8; rateRefresh is the monitor's actual rate (works even
    // without a specific HMONITOR: DWM composes one desktop timing per the primary adapter's output).
    DWM_TIMING_INFO info{};
    info.cbSize = sizeof(info);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.rateRefresh.uiDenominator)
    {
        const double hz = static_cast<double>(info.rateRefresh.uiNumerator) / info.rateRefresh.uiDenominator;
        if (hz >= 20.0 && hz <= 1000.0)
            return hz;
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi))
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 20)
            return static_cast<double>(dm.dmDisplayFrequency);
    }
    return 60.0; // Windows reports 0 or 1 for "default"; 60 Hz is the safe assumption
}

struct Clock
{
    std::mutex mutex;
    std::condition_variable wake;    // the UI thread signals this when a window (re)registers
    std::unordered_set<HWND> active; // windows with a tick in flight / due next frame
    bool running = false;             // the worker thread exists and is parked on `wake` or the timer
    bool stop = false;
    bool remeasure = false;           // WM_DISPLAYCHANGE: the next frame re-reads the rate even on the same monitor
    std::thread worker;
};
std::atomic<unsigned> g_frameCount{ 0 }; // DebugFrameCount(): bumped once per delivered animation frame (test mode only reads it, but it's one atomic increment so always kept up to date)
Clock& Get()
{
    static Clock c;
    return c;
}

void WorkerMain()
{
    Clock& c = Get();
    HANDLE timer = CreateFrameTimer();
    double hz = 60.0;
    HMONITOR lastMonitor = nullptr;
    for (;;)
    {
        std::vector<HWND> frame;
        {
            std::unique_lock<std::mutex> lock(c.mutex);
            c.wake.wait(lock, [&] { return c.stop || !c.active.empty(); });
            if (c.stop)
                break;
            frame.assign(c.active.begin(), c.active.end());
        }
        // Pace to the first live window's monitor (good enough: FastFiles has one window on screen typically;
        // a multi-monitor setup still gets a correct-enough rate since most refresh rates match within the task's
        // tolerance, and each window re-measures every frame it's actually visible on a different one).
        HMONITOR mon = frame.empty() ? nullptr : MonitorFromWindow(frame.front(), MONITOR_DEFAULTTOPRIMARY);
        bool force = false;
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            force = c.remeasure;
            c.remeasure = false;
        }
        if (mon != lastMonitor || hz <= 0.0 || force)
        {
            hz = MonitorRefreshHz(mon);
            lastMonitor = mon;
        }
        // DwmFlush blocks until the next vblank the DWM will actually compose with (Windows 8+: always available),
        // so it alone is the pacing: it already waits one frame interval, no extra sleep before it. A waitable
        // timer wait here too would not run in parallel with DwmFlush's own wait - the two blocking calls would
        // add up serially instead of both landing on the same vblank, roughly halving the delivered frame rate
        // (measured: ~29 fps on a 60 Hz panel until this was removed). The timer is only DwmFlush's fallback, for
        // the (unobserved in practice) case DwmFlush itself fails.
        if (FAILED(DwmFlush()))
        {
            if (timer)
            {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(10000000.0 / hz);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            }
            else
            {
                Sleep(static_cast<DWORD>(1000.0 / hz));
            }
        }
        g_frameCount.fetch_add(1, std::memory_order_relaxed); // test diagnostics: one vblank actually paced out
        for (HWND h : frame)
            if (IsWindow(h))
                PostMessageW(h, kMsgTick, 0, 0);
        {
            // Coalesce: don't post again to a window until it re-registers from its own kMsgTick handler (one
            // SmoothScroll::Tick call), so a slow UI thread never gets a backlog of frame messages queued up.
            std::lock_guard<std::mutex> lock(c.mutex);
            for (HWND h : frame)
                c.active.erase(h);
        }
    }
    if (timer)
        CloseHandle(timer);
}
}

void Register(HWND hwnd)
{
    if (!hwnd)
        return;
    Clock& c = Get();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.active.insert(hwnd);
    if (!c.running)
    {
        c.running = true;
        c.stop = false;
        c.worker = std::thread(WorkerMain);
    }
    c.wake.notify_one();
}

void NotifyDisplayChange()
{
    // A monitor's refresh rate itself can change (Display settings) without the paced window moving to a
    // different HMONITOR, which is the only other trigger WorkerMain re-measures on; this forces the next frame
    // (if one is running) to re-read DwmGetCompositionTimingInfo / EnumDisplaySettings regardless.
    Clock& c = Get();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.remeasure = true;
}

void Forget(HWND hwnd)
{
    Clock& c = Get();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.active.erase(hwnd);
    // The worker thread itself keeps running (parked on `wake`) between animations: tearing it down and
    // recreating it on every scroll would cost more than the idle thread does, and it never busy-spins or holds
    // a timer armed while `active` is empty.
}

double RefreshRateFor(HWND hwnd)
{
    return MonitorRefreshHz(hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr);
}

unsigned DebugFrameCount()
{
    return g_frameCount.load(std::memory_order_relaxed);
}
}
