#pragma once
#include "Common.h"

// Test mode only (Sandbox active and FF_LAYOUT_TRACE set): the jitter of a layout step. Around a block that moves or
// resizes windows it takes the window's pixels before (A), right after (B: what Windows' copy of a moved window's old
// pixels left on screen until the repaint) and after a full repaint (C), each where it is on the screen. A pixel of B
// that is neither A nor C at that spot of the screen was shown nowhere before or after: a part drawn one step off (the
// toolbar's right end on a splitter drag, the right side of the window when its left edge is dragged). One line per
// step to <data>layout-trace.txt: "<tag> ghost <pixels> <box> <window classes at the box>". Nested steps count once
// (the outermost). Without the variable it does nothing.
namespace LayoutProbe
{
bool Start(HWND hwnd, const wchar_t* tag); // true: a Finish() must follow
void Finish();
// The window right after Windows gave it its new size, before it was laid out for it: what a frame drag shows until
// the layout and its paint are done. Finish counts the pixels the size uncovered that this frame left unpainted
// (black, which no surface of either theme is) while the full repaint has something there: "<tag> blank <pixels>"
void Uncovered(HWND hwnd);

struct Scope
{
    Scope(HWND hwnd, const wchar_t* tag) : started(Start(hwnd, tag)) {}
    ~Scope() { if (started) Finish(); }
    const bool started;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}
