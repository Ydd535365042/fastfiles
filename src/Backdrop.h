#pragma once
#include "Theme.h"

// Files' backdrop material (AppSystemBackdrop: MicaController Base / BaseAlt), done by FastFiles itself so it looks
// the same on every Windows version: the desktop wallpaper of each monitor, blurred, then WinUI's luminosity and tint
// layers (MicaController's default recipes), then Files' background color. (No acrylic: it shows the windows behind.) Custom-drawn surfaces paint the palette's chrome / layer / toolbar colors as keys;
// BufferedPaint swaps those pixels for the material at their screen position (the translucent WinUI layers on top).
// Like WinUI, an inactive window shows the solid fallback (Files drives IsInputActive from Activated).
namespace Backdrop
{
enum Material { Solid = 0, Mica = 3, MicaAlt = 4 }; // Files' BackdropMaterialType values
constexpr UINT kMsgReady = WM_APP + 74; // to the main window: the wallpaper is loaded (wParam generation, lParam image)

// The main window (its root gets the "FastFiles.Backdrop" property: only its surfaces are composed)
void Configure(HWND window, int material, bool dark, uint32_t tint);
bool OnReady(WPARAM wp, LPARAM lp); // true when the image was taken (re-apply the theme)

// Files' AppThemeBackgroundImage (MainPage: an Image under the tab bar, address bar and content, Grid.RowSpan 3):
// fit 0 无 / 1 填充 / 2 均匀 / 3 均匀填充, alignment 0 start / 1 center / 2 end, opacity 10..100. Drawn over the
// material (or the solid page color) and under the translucent layers. An empty path removes it.
constexpr UINT kMsgImage = WM_APP + 75; // to the main window: the background image is decoded (wParam generation)
void SetImage(const std::wstring& path, int fit, int vertical, int horizontal, int opacity);
bool OnImage(WPARAM wp, LPARAM lp); // true when it was taken (re-apply the theme)
bool HasImage();
COLORREF ImageMean(); // the image's average (for the palette's averages)
bool Active();        // something to compose: a live material or a background image
void SetBase(COLORREF base); // the solid page color under the image when no material is live
void Reload();                      // wallpaper, monitors or DPI changed
void SetActive(bool active);
void SetMoving(bool moving); // the window's frame is dragged (WM_ENTERSIZEMOVE / EXITSIZEMOVE): the material's cache kept, read shifted
bool Live();                        // a material, its image loaded, the window active
COLORREF Mean(const RECT& screen);  // the material's average over a screen rectangle

// A key color and the translucent fills (WinUI layers) over the material where it is painted, bottom first (the file
// list's cards stack the most: nav layer, file area card, card fill, hover / stroke, the picture box)
constexpr int kLayerFills = 5;
struct Layer
{
    COLORREF key = 0;
    COLORREF fill[kLayerFills]{};
    int pct[kLayerFills]{};
};
void SetLayers(const std::vector<Layer>& layers);
// Swaps the key pixels of a painted 32bpp top-down buffer (0x00RRGGBB) inside rc (client coordinates of hwnd).
// extra: keys only this surface has (the file list's card and its row fills), checked after the window-wide ones.
// paint: the BeginPaint DC; only the parts of rc in its visible region are composed (not the children's rectangles a
// parent's rcPaint spans: the blit is clipped to that region, and the children compose their own pixels).
void Compose(HWND hwnd, uint32_t* bits, int width, const RECT& rc, const std::vector<Layer>* extra = nullptr, HDC paint = nullptr);
}
