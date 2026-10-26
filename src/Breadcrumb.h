#pragma once
#include "Theme.h"

struct BreadcrumbEvents
{
    virtual void OnCrumbNavigate(PCIDLIST_ABSOLUTE pidl, bool newTab) = 0;
    virtual void OnCrumbEdit() = 0;
    virtual void OnCrumbMenu(PCIDLIST_ABSOLUTE pidl, POINT screen) = 0; // right-click on a segment
};

// Files-style path bar: clickable segments, a chevron after each listing its subfolders, leading segments collapse into "…".
class Breadcrumb
{
public:
    bool Create(HWND parent, int id, BreadcrumbEvents* events);
    HWND Hwnd() const { return m_hwnd; }
    void SetLocation(PCIDLIST_ABSOLUTE pidl);
    void SetTheme(const Theme* theme);

private:
    struct Segment
    {
        UniquePidl pidl;
        std::wstring name;
        int textWidth = 0;
        RECT text{};
        RECT chevron{};
    };
    enum class Part { None, Overflow, Text, Chevron, Blank };
    struct Hit { Part part = Part::None; int index = -1; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Measure();
    void Arrange();
    void Paint(HDC hdc);
    Hit HitTest(POINT pt) const;
    void ShowChildren(int index);
    void ShowOverflow();
    void Navigate(PCIDLIST_ABSOLUTE pidl, bool newTab);

    // Files' PathBoxItem drop: dragging over a segment opens it after a pause, dropping moves/copies into it
    struct DropTarget final : IDropTarget
    {
        Breadcrumb* owner = nullptr;
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }  // lives as long as the breadcrumb
        IFACEMETHODIMP_(ULONG) Release() override { return 1; }
        IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
        IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
        IFACEMETHODIMP DragLeave() override;
        IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    };
    void UpdateDrop(POINTL screen, DWORD keys, DWORD* effect);
    void ClearDrop();
    bool CanDropOn(int index) const; // not the folder the items are already in, not into one of them
    static constexpr UINT_PTR kHoverTimer = 1;
    static constexpr UINT kHoverMs = 1300; // Files: Constants.DragAndDrop.HoverToOpenTimespan
    DropTarget m_dropTarget;
    ComPtr<IDropTargetHelper> m_dropHelper;
    ComPtr<IDataObject> m_dragData;
    std::vector<UniquePidl> m_dragItems;
    ComPtr<IDropTarget> m_itemTarget; // the segment folder's own drop target
    DWORD m_dragKeys = 0;             // last DragEnter / DragOver keys (Drop's have lost the mouse button)
    int m_dropIndex = -1;             // segment m_itemTarget belongs to
    int m_hoverIndex = -1;            // segment the hover-to-open timer is running for

    HWND m_hwnd = nullptr;
    BreadcrumbEvents* m_events = nullptr;
    const Theme* m_theme = nullptr;
    HIMAGELIST m_images = nullptr;
    int m_icon = -1;
    RECT m_iconRect{};
    std::vector<Segment> m_segments;
    int m_first = 0; // first visible segment; earlier ones live in the overflow menu
    RECT m_overflow{};
    Hit m_hot;
    Hit m_pressed; // the part the left button went down on (its pressed look)
    bool m_tracking = false;
};
