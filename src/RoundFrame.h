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
    // A layered window and what its bitmap was last made for (size, active state, look)
    struct Piece
    {
        HWND hwnd = nullptr;
        SIZE painted{ -1, -1 };
        bool active = false;
        unsigned version = ~0u;
    };
    enum { kTop, kBottom, kLeft, kRight, kStrips };
    static LRESULT CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
    // One strip of the frame: `strip` within the whole frame (its size `frame`), placed with the frame at `at`
    void PaintStrip(Piece& piece, const RECT& strip, POINT at, SIZE frame);
    // One corner's anti-alias patch: the corner box at (x0, y0) of the window `rc`, `box` square
    void PaintCap(Piece& piece, const RECT& rc, int x0, int y0, int box, bool bottom);
    bool Fresh(const Piece& piece, SIZE key) const;
    void Painted(Piece& piece, SIZE key);
    void HidePieces();
    int Band() const { return MulDiv(8, static_cast<int>(m_dpi), 96); } // the resize band outside the edge
    // The frame around the edge: the band (resizable: every pixel of it takes the mouse), else a wider shadow that
    // lets the mouse through
    int Margin() const { return m_resizable ? Band() : 2 * Band(); }
    // The frame and the corner patch only carry pixels along the edge, so they're split into windows that cover just
    // that: four strips around the edge (top / bottom: the full width with the corners; left / right: between them)
    // and four corner boxes. A resize then repaints a few thin strips, never a window-sized bitmap.
    // m_anchor: a hidden window owning the strips (an owned window: not taken for the process's main window).
    // The corner boxes: owned by `window` itself so they always stay directly above it (click-through)
    HWND m_window = nullptr, m_anchor = nullptr;
    Piece m_frame[kStrips], m_cap[4];
    bool m_resizable = false, m_active = true, m_suppressed = false, m_regionRounded = false, m_updating = false;
    bool m_frameTopmost = false; // the strips' and m_anchor's own band, kept matched to m_window's (see Update())
    UINT m_dpi = 96;
    Look m_look;
    unsigned m_lookVersion = 0;
    SIZE m_regionSize{ -1, -1 };
    HRGN m_capRegion = nullptr;  // a copy of the window's region, kept to test pixels against (PtInRegion)
};
