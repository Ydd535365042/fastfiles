#pragma once
#include "Common.h"

// The geometry and selection of the own file list's items (FileItems.cpp), for every layout Files has: rows (details,
// columns) or cells wrapping into lines (grid, cards, list), split into groups under a header each (Files' GroupStyle:
// headers aren't sticky or collapsible). FileList owns the window, paints and routes the input; this has no window.
// Rows are display positions (FileList::m_order); content coordinates start at the top left of the scrolled area.

struct ItemsGroup
{
    int first = 0, count = 0; // rows
};

struct ItemsMetrics
{
    bool wrap = false;        // cells wrapping into lines (grid / cards / list) instead of one row per line
    bool vertical = false;    // wrap: cells fill columns top to bottom, the columns scroll sideways (Files' list layout,
                              // ItemsWrapGrid Orientation Vertical); otherwise they fill rows left to right
    int itemWidth = 0;        // a cell's width (rows: ignored, they span `rowWidth`)
    int itemHeight = 0;
    int rowWidth = 0;         // rows: their width (the columns' total or the client width, whichever is wider)
    int spacingX = 0, spacingY = 0;
    int padLeft = 0, padTop = 0, padRight = 0, padBottom = 0;
    int headerHeight = 0;     // a group header's line (0: no groups shown)
};

class ItemsLayout
{
public:
    // `groups` cover rows 0..count-1 in order (empty: one group without a header)
    void Build(int count, const std::vector<ItemsGroup>& groups, const ItemsMetrics& m, int clientWidth, int clientHeight = 0);
    int Count() const { return m_count; }
    int ContentWidth() const { return m_contentWidth; }
    int ContentHeight() const { return m_contentHeight; }
    const ItemsMetrics& Metrics() const { return m_m; }

    RECT ItemRect(int row) const;           // content coordinates (empty when out of range)
    int GroupCount() const { return static_cast<int>(m_groups.size()); }
    const ItemsGroup& Group(int g) const { return m_groups[static_cast<size_t>(g)]; }
    RECT HeaderRect(int g) const;           // empty without headers
    int GroupOf(int row) const;
    int HitTest(POINT content) const;       // row under the point, -1 none
    std::vector<int> RowsIn(const RECT& content) const; // rows whose cells meet the rectangle
    std::vector<int> HeadersIn(const RECT& content) const;
    int Columns() const { return m_columns; } // cells per line

    // Keyboard: the row a key moves the focus to from `from` (VK_UP / DOWN / LEFT / RIGHT / PRIOR / NEXT / HOME / END);
    // `page` is the client height. Returns `from` when it can't move.
    int Move(int from, UINT key, int page) const;

private:
    // A row of cells (or, vertical, a column of them): its first row, how many, its top (vertical: its left) and, vertical,
    // the top its first cell starts at (under the group's header)
    struct Line { int first = 0, count = 0, top = 0, start = 0; };
    bool Vertical() const { return m_m.wrap && m_m.vertical; }
    int LineOf(int row) const;
    ItemsMetrics m_m;
    int m_count = 0, m_columns = 1, m_contentWidth = 0, m_contentHeight = 0;
    std::vector<ItemsGroup> m_groups;
    std::vector<int> m_headerTop;           // per group (-1: no header)
    std::vector<int> m_headerLeft, m_headerWidth; // vertical: each header over its group's columns
    std::vector<Line> m_lines;
    std::vector<int> m_lineOfRow;
};

// Which rows are selected, the focus and the anchor of a Shift range (Files' ListViewBase SelectionMode Extended)
class ItemsSelection
{
public:
    void Reset(int count);
    int Count() const { return static_cast<int>(m_on.size()); }
    bool Selected(int row) const { return row >= 0 && row < Count() && m_on[static_cast<size_t>(row)]; }
    int SelectedCount() const { return m_selected; }
    void Set(int row, bool on);
    void Range(int a, int b, bool on);
    void All(bool on);
    void Invert();
    int Next(int after) const;              // next selected row after `after` (-1 from the start), -1 at the end
    const std::vector<unsigned char>& Bits() const { return m_on; }
    void SetBits(const std::vector<unsigned char>& bits);
    int focus = -1, anchor = -1;
    unsigned version = 0;                   // bumped by every change (caches of the selection compare it)

private:
    std::vector<unsigned char> m_on;
    int m_selected = 0;
};
