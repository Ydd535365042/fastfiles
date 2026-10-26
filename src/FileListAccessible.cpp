#include "FileList.h"
#include <oleacc.h>

// What a screen reader sees of the own list (the items window draws everything itself): a list of items named as
// shown, with their selected / focused states, places and the default action (open). Children are rows + 1.

namespace
{
class Enum final : public IEnumVARIANT
{
public:
    explicit Enum(std::vector<long> ids) : m_ids(std::move(ids)) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumVARIANT)
        {
            *ppv = static_cast<IEnumVARIANT*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --m_refs;
        if (!r)
            delete this;
        return r;
    }
    IFACEMETHODIMP Next(ULONG count, VARIANT* out, ULONG* fetched) override
    {
        ULONG n = 0;
        for (; n < count && m_at < m_ids.size(); ++n, ++m_at)
        {
            VariantInit(&out[n]);
            out[n].vt = VT_I4;
            out[n].lVal = m_ids[m_at];
        }
        if (fetched)
            *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG count) override
    {
        m_at = std::min(m_ids.size(), m_at + count);
        return m_at < m_ids.size() ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override
    {
        m_at = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumVARIANT** out) override
    {
        if (!out)
            return E_POINTER;
        auto* copy = new Enum(m_ids);
        copy->m_at = m_at;
        *out = copy;
        return S_OK;
    }

private:
    ULONG m_refs = 1;
    std::vector<long> m_ids;
    size_t m_at = 0;
};
}
class FileList::Accessible final : public IAccessible
{
public:
    explicit Accessible(FileList* owner) : m_owner(owner)
    {
        CreateStdAccessibleObject(owner->m_list, OBJID_CLIENT, IID_PPV_ARGS(&m_std)); // the window parts (parent, focus moves)
    }
    void Disconnect() { m_owner = nullptr; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch || riid == IID_IAccessible)
        {
            *ppv = static_cast<IAccessible*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG r = --m_refs;
        if (!r)
            delete this;
        return r;
    }
    IFACEMETHODIMP GetTypeInfoCount(UINT* n) override { if (n) *n = 0; return S_OK; }
    IFACEMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }

    IFACEMETHODIMP get_accParent(IDispatch** out) override { return m_std ? m_std->get_accParent(out) : E_FAIL; }
    IFACEMETHODIMP get_accChildCount(long* count) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        *count = m_owner->Count();
        return S_OK;
    }
    IFACEMETHODIMP get_accChild(VARIANT, IDispatch** out) override
    {
        *out = nullptr;
        return S_FALSE; // rows are simple elements
    }
    IFACEMETHODIMP get_accName(VARIANT child, BSTR* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        *out = nullptr;
        if (child.vt != VT_I4)
            return E_INVALIDARG;
        if (child.lVal == CHILDID_SELF)
            return m_std ? m_std->get_accName(child, out) : S_FALSE;
        const int row = child.lVal - 1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        *out = SysAllocString(m_owner->CellText(row, ColName).c_str());
        return S_OK;
    }
    IFACEMETHODIMP get_accValue(VARIANT, BSTR* out) override { *out = nullptr; return S_FALSE; }
    IFACEMETHODIMP get_accDescription(VARIANT child, BSTR* out) override
    {
        // The other columns as one line (修改日期, 类型, 大小), as the details view reads them
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        *out = nullptr;
        if (child.vt != VT_I4 || child.lVal == CHILDID_SELF)
            return S_FALSE;
        const int row = child.lVal - 1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        std::wstring text;
        for (int c : { ColModified, ColType, ColSize })
            if (std::wstring s = m_owner->CellText(row, c); !s.empty())
                text += (text.empty() ? L"" : L", ") + s;
        *out = SysAllocString(text.c_str());
        return S_OK;
    }
    IFACEMETHODIMP get_accRole(VARIANT child, VARIANT* out) override
    {
        VariantInit(out);
        out->vt = VT_I4;
        out->lVal = child.vt == VT_I4 && child.lVal == CHILDID_SELF ? ROLE_SYSTEM_LIST : ROLE_SYSTEM_LISTITEM;
        return S_OK;
    }
    IFACEMETHODIMP get_accState(VARIANT child, VARIANT* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        VariantInit(out);
        out->vt = VT_I4;
        const bool focused = GetFocus() == m_owner->m_list;
        if (child.vt == VT_I4 && child.lVal == CHILDID_SELF)
        {
            out->lVal = STATE_SYSTEM_FOCUSABLE | (focused ? STATE_SYSTEM_FOCUSED : 0);
            return S_OK;
        }
        const int row = child.lVal - 1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        RECT client{}, r = m_owner->RowRect(row);
        GetClientRect(m_owner->m_list, &client);
        RECT both{};
        long s = STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_MULTISELECTABLE | STATE_SYSTEM_EXTSELECTABLE;
        if (m_owner->m_sel.Selected(row))
            s |= STATE_SYSTEM_SELECTED;
        if (row == m_owner->FocusedIndex() && focused)
            s |= STATE_SYSTEM_FOCUSED;
        if (!IntersectRect(&both, &client, &r))
            s |= STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_INVISIBLE;
        out->lVal = s;
        return S_OK;
    }
    IFACEMETHODIMP get_accHelp(VARIANT, BSTR* out) override { *out = nullptr; return S_FALSE; }
    IFACEMETHODIMP get_accHelpTopic(BSTR* file, VARIANT, long* topic) override { *file = nullptr; *topic = 0; return S_FALSE; }
    IFACEMETHODIMP get_accKeyboardShortcut(VARIANT, BSTR* out) override { *out = nullptr; return S_FALSE; }
    IFACEMETHODIMP get_accFocus(VARIANT* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        VariantInit(out);
        if (GetFocus() != m_owner->m_list)
            return S_FALSE;
        out->vt = VT_I4;
        out->lVal = m_owner->FocusedIndex() >= 0 ? m_owner->FocusedIndex() + 1 : CHILDID_SELF;
        return S_OK;
    }
    IFACEMETHODIMP get_accSelection(VARIANT* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        VariantInit(out);
        const std::vector<int> rows = m_owner->SelectedRows();
        if (rows.empty())
            return S_FALSE;
        if (rows.size() == 1)
        {
            out->vt = VT_I4;
            out->lVal = rows[0] + 1;
            return S_OK;
        }
        std::vector<long> ids;
        for (int r : rows)
            ids.push_back(r + 1);
        out->vt = VT_UNKNOWN;
        out->punkVal = new Enum(std::move(ids));
        return S_OK;
    }
    IFACEMETHODIMP get_accDefaultAction(VARIANT child, BSTR* out) override
    {
        *out = child.vt == VT_I4 && child.lVal != CHILDID_SELF ? SysAllocString(L"Open") : nullptr;
        return *out ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP accSelect(long flags, VARIANT child) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        const int row = child.vt == VT_I4 ? child.lVal - 1 : -1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        m_owner->UserSelected();
        if (flags & SELFLAG_TAKEFOCUS)
            SetFocus(m_owner->m_list);
        if (flags & SELFLAG_TAKESELECTION)
            m_owner->m_sel.All(false), m_owner->m_sel.Set(row, true);
        if (flags & SELFLAG_ADDSELECTION)
            m_owner->m_sel.Set(row, true);
        if (flags & SELFLAG_REMOVESELECTION)
            m_owner->m_sel.Set(row, false);
        if (flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION))
            m_owner->m_sel.focus = m_owner->m_sel.anchor = row;
        m_owner->EnsureVisible(row);
        m_owner->SelectionChanged();
        return S_OK;
    }
    IFACEMETHODIMP accLocation(long* x, long* y, long* w, long* h, VARIANT child) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        if (child.vt == VT_I4 && child.lVal == CHILDID_SELF)
            return m_std ? m_std->accLocation(x, y, w, h, child) : E_FAIL;
        const int row = child.vt == VT_I4 ? child.lVal - 1 : -1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        RECT r = m_owner->RowRect(row);
        MapWindowPoints(m_owner->m_list, nullptr, reinterpret_cast<POINT*>(&r), 2);
        *x = r.left, *y = r.top, *w = r.right - r.left, *h = r.bottom - r.top;
        return S_OK;
    }
    IFACEMETHODIMP accNavigate(long dir, VARIANT start, VARIANT* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        VariantInit(out);
        const int n = m_owner->Count();
        const int from = start.vt == VT_I4 ? start.lVal - 1 : -1;
        int to = -1;
        if (dir == NAVDIR_FIRSTCHILD && n > 0) to = 0;
        else if (dir == NAVDIR_LASTCHILD && n > 0) to = n - 1;
        else if (from >= 0 && (dir == NAVDIR_NEXT || dir == NAVDIR_DOWN) && from + 1 < n) to = from + 1;
        else if (from > 0 && (dir == NAVDIR_PREVIOUS || dir == NAVDIR_UP)) to = from - 1;
        if (to < 0)
            return from < 0 && m_std ? m_std->accNavigate(dir, start, out) : S_FALSE;
        out->vt = VT_I4;
        out->lVal = to + 1;
        return S_OK;
    }
    IFACEMETHODIMP accHitTest(long x, long y, VARIANT* out) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        VariantInit(out);
        POINT pt{ x, y };
        ScreenToClient(m_owner->m_list, &pt);
        RECT client{};
        GetClientRect(m_owner->m_list, &client);
        if (!PtInRect(&client, pt))
            return S_FALSE;
        const int row = m_owner->RowAt(pt);
        out->vt = VT_I4;
        out->lVal = row >= 0 ? row + 1 : CHILDID_SELF;
        return S_OK;
    }
    IFACEMETHODIMP accDoDefaultAction(VARIANT child) override
    {
        if (!m_owner)
            return CO_E_OBJNOTCONNECTED;
        const int row = child.vt == VT_I4 ? child.lVal - 1 : -1;
        if (row < 0 || row >= m_owner->Count())
            return E_INVALIDARG;
        if (!m_owner->m_sel.Selected(row))
            m_owner->FocusRow(row, true, false, false);
        m_owner->OpenSelection();
        return S_OK;
    }
    IFACEMETHODIMP put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }

private:
    ULONG m_refs = 1;
    FileList* m_owner;
    ComPtr<IAccessible> m_std;
};

LRESULT FileList::OnGetObject(WPARAM wp, LPARAM lp)
{
    if (static_cast<long>(lp) != OBJID_CLIENT)
        return DefWindowProcW(m_list, WM_GETOBJECT, wp, lp);
    if (!m_accessible)
        m_accessible = new Accessible(this);
    return LresultFromObject(IID_IAccessible, wp, m_accessible);
}

void FileList::ReleaseAccessible()
{
    if (m_accessible)
    {
        m_accessible->Disconnect();
        m_accessible->Release();
        m_accessible = nullptr;
    }
}
