#pragma once
#include "Theme.h"

// Files' AppearancePage background color button opens a Flyout holding a CommunityToolkit ColorPicker with
// IsAlphaEnabled="True" and IsColorSpectrumVisible="False" (AppearancePage.xaml). With the spectrum off, WinUI's
// ColorPicker shows the color preview, a hex box, and R/G/B/A sliders. FastFiles draws the same thing natively:
// Theme tokens only (checkerLight/checkerDark for the transparency checkerboard, sliderThumb/scrubTrack for the
// slider look, shared with MediaPreview's scrub bar).
namespace ColorPickerFlyout
{
// anchor: screen point to open near (the button under the cursor); initial: current #AARRGGBB.
// Returns true and fills `result` (#AARRGGBB) when the user accepted a color; false when dismissed (Esc / outside click).
bool Show(HWND owner, const Theme* theme, POINT anchor, uint32_t initial, uint32_t& result);
}
