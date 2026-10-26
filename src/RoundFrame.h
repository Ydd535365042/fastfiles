#pragma once
#include <windows.h>

// Rounded window corners like Windows 11's (Files' window, the setup's): on Windows 11 DWM rounds them
// (DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND, 8px). Windows 8 / 10 draw square windows, so there the window
// gets a rounded region and a layered window just behind it draws what DWM no longer can: the 1px border around
// the rounded edge, a soft shadow and, for a resizable window, the resize band outside the edge (its pixels keep
// alpha 1 at least, so the mouse finds them; past the band everything is click-through). Maximized, minimized,
// hidden or suppressed (full screen): square, no frame window. Used by FastFiles (src/) and the setup (setup/).
// SetWindowRgn's rounded-rect region is a hard 1-bit clip (no anti-aliasing), so the window's own corner pixels
// show stair steps even with the ring behind it hiding the background. A second layered window, owned directly by
// `window` (Windows keeps an owned window immediately above its owner, so it always sits over the jagged edge,
// click-through), repaints just those few corner pixels: where the region wrongly kept a pixel outside the ideal
// curve it covers it with the same ring/shadow look that belongs there; where the region wrongly dropped a pixel
// inside the curve it fills it back in with the window's own flat corner color (`Look::fillTop/fillBottom`).
class RoundFrame
{
public:
    struct Look
    {
        COLORREF border = 0, borderInactive = 0; // the 1px ring (premultiplied at its alpha)
        int borderAlpha = 0, borderInactiveAlpha = 0; // 0..255
        int shadowAlpha = 0, shadowInactiveAlpha = 0; // the shadow's strongest alpha (black), 0..255
        // The window's own flat background at the top corners (title / tab row) and bottom corners (everything
        // else fills to the edge there too): what the anti-alias cap blends toward over the ring/shadow look
        COLORREF fillTop = 0, fillBottom = 0;
    };
    static bool Windows11();     // build 22000+: DWM rounds the corners itself
    ~RoundFrame();
    // `resizable`: the band outside the edge sizes the window (WM_SYSCOMMAND SC_SIZE to it)
    void Attach(HWND window, bool resizable);
    void SetLook(const Look& look);
    void SetDpi(UINT dpi);
    void SetActive(bool active);
    void Suppress(bool on);      // full screen: square, no frame window
    // The window moved, sized, showed, hid, changed z-order (WM_WINDOWPOSCHANGED) or its state changed
    void Update();
    void Detach();
    bool Custom() const { return m_window && !Windows11(); } // this draws the frame (the window has no system one)
    bool Rounded() const;        // corners rounded now (not maximized / minimized / suppressed)
    int Radius() const { return MulDiv(8, static_cast<int>(m_dpi), 96); }

private:
    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(POINT at, SIZE size);
    void PaintCap(const RECT& rc);  // the above-window anti-alias patch over the region's stepped corners
    int Band() const { return MulDiv(8, static_cast<int>(m_dpi), 96); } // the resize band outside the edge
    // The frame window around the edge: the band (resizable: every pixel of it takes the mouse), else a wider shadow
    // that lets the mouse through
    int Margin() const { return m_resizable ? Band() : 2 * Band(); }
    // m_anchor: a hidden window owning the frame window (an owned window: not taken for the process's main window).
    // m_cap: owned by `window` itself so it always stays directly above it (click-through corner anti-aliasing)
    HWND m_window = nullptr, m_frame = nullptr, m_anchor = nullptr, m_cap = nullptr;
    bool m_resizable = false, m_active = true, m_suppressed = false, m_regionRounded = false, m_updating = false;
    bool m_frameTopmost = false; // m_frame/m_anchor's own band, kept matched to m_window's (see Update())
    UINT m_dpi = 96;
    Look m_look;
    SIZE m_painted{ -1, -1 };    // the frame bitmap's size, active state and look when last made
    bool m_paintedActive = false;
    unsigned m_lookVersion = 0, m_paintedVersion = ~0u;
    SIZE m_regionSize{ -1, -1 };
    HRGN m_capRegion = nullptr;  // a copy of the window's region, kept to test pixels against (PtInRegion)
    SIZE m_capPainted{ -1, -1 }; // the cap bitmap's size, active state and look when last made
    bool m_capPaintedActive = false;
    unsigned m_capPaintedVersion = ~0u;
};
