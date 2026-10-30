#include "MainWindow.h"
#include "LayoutProbe.h"

// Dual pane (Files' "dual pane" / ShellPanesPage): each tab slot holds one or two panes,
// side by side or stacked, with a draggable splitter and an accent bar over the focused pane.

void MainWindow::PaneRects(const TabSlot& slot, RECT out[2], RECT* split) const
{
    const RECT r = m_viewRect;
    const int gap = Scale(kPaneGap, m_dpi), bar = Scale(kPaneBar, m_dpi);
    if (slot.horizontal)
    {
        const int first = std::max(0, MulDiv(r.bottom - r.top - gap, slot.ratio, 1000));
        out[0] = { r.left, r.top + bar, r.right, r.top + first };
        out[1] = { r.left, r.top + first + gap + bar, r.right, r.bottom };
        if (split)
            *split = { r.left, r.top + first, r.right, r.top + first + gap };
    }
    else
    {
        const int first = std::max(0, MulDiv(r.right - r.left - gap, slot.ratio, 1000));
        out[0] = { r.left, r.top + bar, r.left + first, r.bottom };
        out[1] = { r.left + first + gap, r.top + bar, r.right, r.bottom };
        if (split)
            *split = { r.left + first, r.top, r.left + first + gap, r.bottom };
    }
    for (int i = 0; i < 2; ++i)
    {
        out[i].bottom = std::max(out[i].top, out[i].bottom);
        out[i].right = std::max(out[i].left, out[i].right);
    }
}

void MainWindow::PlacePanes()
{
    m_paneSplit = {};
    TabSlot* slot = ActiveSlot();
    if (!slot)
        return;
    const LayoutProbe::Scope probe(m_hwnd, L"panes");
    // A pane's first Show goes straight to its home page (no navigation event), so make sure there is data for it
    for (const auto& pane : slot->panes)
        if (pane && IsHomePidl(pane->Location()))
        {
            EnsureHomeFresh();
            break;
        }
    if (!slot->Split())
    {
        slot->panes[0]->Show(m_viewRect);
        return;
    }
    RECT rc[2];
    PaneRects(*slot, rc, &m_paneSplit);
    for (int i = 0; i < 2; ++i)
        slot->panes[i]->Show(rc[i]);
    InvalidateRect(m_hwnd, &m_viewRect, FALSE); // bars and splitter
}

void MainWindow::SetPaneFocus(int pane, bool focusView)
{
    TabSlot* slot = ActiveSlot();
    if (!slot || !slot->Split() || pane < 0 || pane > 1)
        return;
    const bool changed = slot->focus != pane;
    slot->focus = pane;
    if (focusView)
        slot->panes[pane]->Focus();
    if (!changed)
        return;
    UpdateTabLabel(m_active);
    UpdateChrome();
    UpdateStatus();
    InvalidateRect(m_hwnd, &m_viewRect, FALSE);
}

// Mouse and keyboard input decide which pane the toolbar, address bar and shortcuts act on
void MainWindow::SyncPaneFocus(HWND hwnd)
{
    TabSlot* slot = ActiveSlot();
    if (!slot || !slot->Split())
        return;
    for (int i = 0; i < 2; ++i)
        if (slot->panes[i]->Contains(hwnd))
        {
            if (i != slot->focus)
                SetPaneFocus(i, false);
            return;
        }
}

void MainWindow::AddPane(PCIDLIST_ABSOLUTE pidl, bool horizontal, bool focus)
{
    TabSlot* slot = ActiveSlot();
    if (!slot || slot->Split())
        return;
    slot->panes[1] = MakeTab(pidl); // null: the home page, Files' default for a new pane
    slot->horizontal = horizontal;
    slot->ratio = 500;
    slot->focus = focus ? 1 : 0;
    PlacePanes();
    slot->Focused()->Focus();
    UpdateTabLabel(m_active);
    UpdateChrome();
    UpdateStatus();
}

void MainWindow::ClosePane(int pane)
{
    TabSlot* slot = ActiveSlot();
    if (!slot || !slot->Split() || pane < 0 || pane > 1)
        return;
    slot->panes[pane]->Close();
    if (pane == 0)
        slot->panes[0] = std::move(slot->panes[1]);
    slot->panes[1].Reset();
    slot->focus = 0;
    PlacePanes();
    slot->panes[0]->Focus();
    UpdateTabLabel(m_active);
    UpdateChrome();
    UpdateStatus();
    InvalidateRect(m_hwnd, &m_viewRect, FALSE);
}

void MainWindow::OpenInOtherPane(PCIDLIST_ABSOLUTE pidl)
{
    TabSlot* slot = ActiveSlot();
    if (!slot || !pidl)
        return;
    if (!slot->Split())
    {
        AddPane(pidl, m_settings.paneHorizontal, true); // Files' ShellPaneArrangementOption
        return;
    }
    const int other = 1 - slot->focus;
    slot->panes[other]->Navigate(pidl);
    SetPaneFocus(other, true);
}

void MainWindow::ToggleDualPane()
{
    TabSlot* slot = ActiveSlot();
    if (!slot)
        return;
    if (slot->Split())
        ClosePane(1);
    else
        AddPane(nullptr, m_settings.paneHorizontal, true); // Files' ToggleDualPane: OpenSecondaryPane(ShellPaneArrangementOption)
}

void MainWindow::SplitPanes(bool horizontal)
{
    TabSlot* slot = ActiveSlot();
    if (!slot)
        return;
    if (!slot->Split())
    {
        AddPane(nullptr, horizontal, true);
        return;
    }
    if (slot->horizontal == horizontal)
        return;
    slot->horizontal = horizontal;
    PlacePanes();
}

bool MainWindow::OverPaneSplitter(int x, int y) const
{
    return m_paneSplit.right > m_paneSplit.left && PtInRect(&m_paneSplit, { x, y });
}
