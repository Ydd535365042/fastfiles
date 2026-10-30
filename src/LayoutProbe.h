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

struct Scope
{
    Scope(HWND hwnd, const wchar_t* tag) : started(Start(hwnd, tag)) {}
    ~Scope() { if (started) Finish(); }
    const bool started;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}
