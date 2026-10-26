#include "FileItems.h"

void ItemsLayout::Build(int count, const std::vector<ItemsGroup>& groupsIn, const ItemsMetrics& m, int clientWidth, int clientHeight)
{
    m_m = m;
    m_count = std::max(0, count);
    m_groups = groupsIn;
    const bool headers = m.headerHeight > 0 && !m_groups.empty();
    if (m_groups.empty())
        m_groups.push_back({ 0, m_count });
    const int cellW = std::max(1, m.itemWidth), stepX = cellW + m.spacingX, stepY = std::max(1, m.itemHeight) + m.spacingY;
    if (Vertical())
    {
        // Columns as tall as the window allows; a group's header over its columns, the next group to the right
        m_lines.clear();
        m_headerTop.assign(m_groups.size(), -1);
        m_headerLeft.assign(m_groups.size(), 0);
        m_headerWidth.assign(m_groups.size(), 0);
        m_lineOfRow.assign(static_cast<size_t>(m_count), 0);
        const int top = m.padTop + (headers ? m.headerHeight : 0);
        m_columns = std::max(1, (clientHeight - top - m.padBottom + m.spacingY) / stepY); // cells per column
        int x = m.padLeft;
        for (size_t g = 0; g < m_groups.size(); ++g)
        {
            const ItemsGroup& group = m_groups[g];
            const int left = x;
            for (int i = 0; i < group.count; i += m_columns)
            {
                Line line{ group.first + i, std::min(m_columns, group.count - i), x, top };
                for (int k = 0; k < line.count; ++k)
                    m_lineOfRow[static_cast<size_t>(line.first + k)] = static_cast<int>(m_lines.size());
                m_lines.push_back(line);
                x += stepX;
            }
            if (headers)
            {
                m_headerTop[g] = m.padTop;
                m_headerLeft[g] = left;
                m_headerWidth[g] = std::max(x - left - m.spacingX, cellW);
                x = std::max(x, left + m_headerWidth[g] + m.spacingX);
            }
        }
        m_contentWidth = (m_lines.empty() ? x : x - m.spacingX) + m.padRight;
        m_contentHeight = clientHeight;
        return;
    }
    m_columns = m.wrap ? std::max(1, (clientWidth - m.padLeft - m.padRight + m.spacingX) / stepX) : 1;
    m_lines.clear();
    m_headerTop.assign(m_groups.size(), -1);
    m_lineOfRow.assign(static_cast<size_t>(m_count), 0);
    int y = m.padTop;
    bool any = false;
    for (size_t g = 0; g < m_groups.size(); ++g)
    {
        const ItemsGroup& group = m_groups[g];
        if (headers)
        {
            if (any)
                y += m.spacingY;
            m_headerTop[g] = y;
            y += m.headerHeight;
        }
        for (int i = 0; i < group.count; i += m_columns)
        {
            Line line{ group.first + i, std::min(m_columns, group.count - i), y };
            for (int k = 0; k < line.count; ++k)
                if (line.first + k < m_count)
                    m_lineOfRow[static_cast<size_t>(line.first + k)] = static_cast<int>(m_lines.size());
            m_lines.push_back(line);
            y += stepY;
            any = true;
        }
        any = any || headers;
    }
    if (!m_lines.empty())
        y -= m.spacingY;
    m_contentHeight = y + m.padBottom;
    m_contentWidth = m.wrap ? clientWidth : m.padLeft + m.rowWidth + m.padRight;
}

int ItemsLayout::LineOf(int row) const
{
    return row >= 0 && row < m_count ? m_lineOfRow[static_cast<size_t>(row)] : -1;
}

RECT ItemsLayout::ItemRect(int row) const
{
    const int l = LineOf(row);
    if (l < 0)
        return {};
    const Line& line = m_lines[static_cast<size_t>(l)];
    const int col = row - line.first;
    if (Vertical())
    {
        const int y = line.start + col * (m_m.itemHeight + m_m.spacingY);
        return { line.top, y, line.top + m_m.itemWidth, y + m_m.itemHeight };
    }
    const int x = m_m.padLeft + (m_m.wrap ? col * (m_m.itemWidth + m_m.spacingX) : 0);
    const int w = m_m.wrap ? m_m.itemWidth : m_m.rowWidth;
    return { x, line.top, x + w, line.top + m_m.itemHeight };
}

RECT ItemsLayout::HeaderRect(int g) const
{
    if (g < 0 || g >= GroupCount() || m_headerTop[static_cast<size_t>(g)] < 0)
        return {};
    const int top = m_headerTop[static_cast<size_t>(g)];
    if (Vertical())
    {
        const int left = m_headerLeft[static_cast<size_t>(g)];
        return { left, top, left + m_headerWidth[static_cast<size_t>(g)], top + m_m.headerHeight };
    }
    const int right = m_m.wrap ? m_contentWidth - m_m.padRight : m_m.padLeft + m_m.rowWidth;
    return { m_m.padLeft, top, std::max(m_m.padLeft, right), top + m_m.headerHeight };
}

int ItemsLayout::GroupOf(int row) const
{
    for (int g = 0; g < GroupCount(); ++g)
        if (row >= m_groups[static_cast<size_t>(g)].first && row < m_groups[static_cast<size_t>(g)].first + m_groups[static_cast<size_t>(g)].count)
            return g;
    return -1;
}

int ItemsLayout::HitTest(POINT p) const
{
    if (m_lines.empty())
        return -1;
    if (Vertical())
    {
        // The last column starting at or left of the point, then the cell in it
        auto it = std::upper_bound(m_lines.begin(), m_lines.end(), p.x, [](int x, const Line& line) { return x < line.top; });
        if (it == m_lines.begin())
            return -1;
        const Line& line = *(it - 1);
        if (p.x >= line.top + m_m.itemWidth || p.y < line.start)
            return -1;
        const int step = m_m.itemHeight + m_m.spacingY, cell = (p.y - line.start) / step;
        return cell < line.count && p.y - line.start - cell * step < m_m.itemHeight ? line.first + cell : -1;
    }
    // The last line starting at or above the point
    auto it = std::upper_bound(m_lines.begin(), m_lines.end(), p.y, [](int y, const Line& line) { return y < line.top; });
    if (it == m_lines.begin())
        return -1;
    const Line& line = *(it - 1);
    if (p.y >= line.top + m_m.itemHeight)
        return -1;
    const int x = p.x - m_m.padLeft;
    if (x < 0)
        return -1;
    if (!m_m.wrap)
        return x < m_m.rowWidth ? line.first : -1;
    const int step = m_m.itemWidth + m_m.spacingX, col = x / step;
    return col < line.count && x - col * step < m_m.itemWidth ? line.first + col : -1;
}

std::vector<int> ItemsLayout::RowsIn(const RECT& r) const
{
    std::vector<int> rows;
    if (Vertical())
    {
        for (const Line& line : m_lines)
        {
            if (line.top >= r.right)
                break;
            if (line.top + m_m.itemWidth <= r.left)
                continue;
            for (int k = 0; k < line.count; ++k)
            {
                const RECT cell = ItemRect(line.first + k);
                if (cell.top < r.bottom && cell.bottom > r.top)
                    rows.push_back(line.first + k);
            }
        }
        return rows;
    }
    for (const Line& line : m_lines)
    {
        if (line.top >= r.bottom)
            break;
        if (line.top + m_m.itemHeight <= r.top)
            continue;
        for (int k = 0; k < line.count; ++k)
        {
            const RECT cell = ItemRect(line.first + k);
            if (cell.left < r.right && cell.right > r.left)
                rows.push_back(line.first + k);
        }
    }
    return rows;
}

std::vector<int> ItemsLayout::HeadersIn(const RECT& r) const
{
    std::vector<int> out;
    for (int g = 0; g < GroupCount(); ++g)
    {
        const RECT h = HeaderRect(g);
        if (h.bottom > h.top && h.top < r.bottom && h.bottom > r.top && h.left < r.right && h.right > r.left)
            out.push_back(g);
    }
    return out;
}

int ItemsLayout::Move(int from, UINT key, int page) const
{
    if (m_count == 0)
        return -1;
    if (from < 0 || from >= m_count)
        return key == VK_END ? m_count - 1 : 0;
    const int l = LineOf(from), col = from - m_lines[static_cast<size_t>(l)].first;
    if (Vertical())
    {
        // Up / down within the column (on into the next one), left / right to the same place in the next column
        auto at = [&](int line) { const Line& t = m_lines[static_cast<size_t>(line)]; return t.first + std::min(col, t.count - 1); };
        const int lines = static_cast<int>(m_lines.size());
        const int perPage = std::max(1, page / std::max(1, m_m.itemWidth + m_m.spacingX)); // (page: the client width here)
        switch (key)
        {
        case VK_HOME: return 0;
        case VK_END: return m_count - 1;
        case VK_UP: return from > 0 ? from - 1 : from;
        case VK_DOWN: return from + 1 < m_count ? from + 1 : from;
        case VK_LEFT: return l > 0 ? at(l - 1) : from;
        case VK_RIGHT: return l + 1 < lines ? at(l + 1) : from;
        case VK_PRIOR: return at(std::max(0, l - perPage));
        case VK_NEXT: return at(std::min(lines - 1, l + perPage));
        default: return from;
        }
    }
    auto inLine = [&](int line)
    {
        const Line& target = m_lines[static_cast<size_t>(line)];
        return target.first + std::min(col, target.count - 1);
    };
    // The line about a page above / below (its top within a page of this one's)
    auto pageLine = [&](int dir)
    {
        const int top = m_lines[static_cast<size_t>(l)].top;
        const int reach = std::max(page - m_m.itemHeight, m_m.itemHeight);
        int line = l;
        while (line + dir >= 0 && line + dir < static_cast<int>(m_lines.size()) &&
            std::abs(m_lines[static_cast<size_t>(line + dir)].top - top) <= reach)
            line += dir;
        return line;
    };
    switch (key)
    {
    case VK_HOME: return 0;
    case VK_END: return m_count - 1;
    case VK_UP: return l > 0 ? inLine(l - 1) : from;
    case VK_DOWN: return l + 1 < static_cast<int>(m_lines.size()) ? inLine(l + 1) : from;
    case VK_LEFT: return m_m.wrap && from > 0 ? from - 1 : from;
    case VK_RIGHT: return m_m.wrap && from + 1 < m_count ? from + 1 : from;
    case VK_PRIOR: return inLine(pageLine(-1));
    case VK_NEXT: return inLine(pageLine(1));
    default: return from;
    }
}

void ItemsSelection::Reset(int count)
{
    m_on.assign(static_cast<size_t>(std::max(0, count)), 0);
    m_selected = 0;
    focus = anchor = -1;
    ++version;
}

void ItemsSelection::Set(int row, bool on)
{
    if (row < 0 || row >= Count() || (m_on[static_cast<size_t>(row)] != 0) == on)
        return;
    m_on[static_cast<size_t>(row)] = on ? 1 : 0;
    m_selected += on ? 1 : -1;
    ++version;
}

void ItemsSelection::Range(int a, int b, bool on)
{
    if (a > b)
        std::swap(a, b);
    for (int i = std::max(0, a); i <= std::min(b, Count() - 1); ++i)
        Set(i, on);
}

void ItemsSelection::All(bool on)
{
    std::fill(m_on.begin(), m_on.end(), static_cast<unsigned char>(on ? 1 : 0));
    m_selected = on ? Count() : 0;
    ++version;
}

void ItemsSelection::Invert()
{
    for (unsigned char& c : m_on)
        c = c ? 0 : 1;
    m_selected = Count() - m_selected;
    ++version;
}

void ItemsSelection::SetBits(const std::vector<unsigned char>& bits)
{
    if (bits.size() != m_on.size() || bits == m_on)
        return;
    m_on = bits;
    m_selected = static_cast<int>(std::count_if(m_on.begin(), m_on.end(), [](unsigned char c) { return c != 0; }));
    ++version;
}

int ItemsSelection::Next(int after) const
{
    for (int i = std::max(0, after + 1); i < Count(); ++i)
        if (m_on[static_cast<size_t>(i)])
            return i;
    return -1;
}
