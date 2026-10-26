#include "Toolbar.h"
#include "Lang.h"
#include "Settings.h"

namespace ToolbarModel
{
static_assert(std::tuple_size_v<decltype(AppSettings::toolbar)> == ContextCount, "one settings line per toolbar context");

namespace
{
std::vector<Command> g_catalog;

// Files' ToolbarSections.DefaultItemsByContext (ShowIcon / ShowLabel as there)
struct Default { const wchar_t* key; bool icon, label; };
const std::vector<Default>& DefaultsOf(int context)
{
    static const std::vector<Default> lists[ContextCount] = {
        { { L"new", true, true }, { L"separator", false, false }, { L"cut", true, false }, { L"copy", true, false }, { L"paste", true, false },
          { L"rename", true, false }, { L"share", true, false }, { L"delete", true, false }, { L"properties", true, false } },
        {}, // Archive: no built-in compress/extract (removed at the user's request; the shell's own menu covers it)
        { { L"editinnotepad", true, true } },
        { { L"runwithpowershell", true, true } },
        { { L"wallpaper", true, true }, { L"rotateleft", true, true }, { L"rotateright", true, true } },
        { { L"playall", true, true } },
        { { L"installfont", true, true } },
        { { L"installdriver", true, true } },
        { { L"installcertificate", true, true } },
        { { L"emptybin", true, true }, { L"restoreallbin", true, true }, { L"restorebin", true, true } },
        {},
    };
    return lists[std::clamp(context, 0, ContextCount - 1)];
}
}

const wchar_t* ContextKey(int context)
{
    static const wchar_t* const keys[ContextCount] = { L"always", L"archive", L"batch", L"powershell", L"image", L"media", L"font",
        L"driver", L"certificate", L"recyclebin", L"other" };
    return keys[std::clamp(context, 0, ContextCount - 1)];
}

const wchar_t* ContextName(int context)
{
    static const wchar_t* const names[ContextCount] = { Tr(L"始终显示"), Tr(L"压缩文件"), Tr(L"批处理文件"), Tr(L"PowerShell 文件"), Tr(L"图片文件"), Tr(L"媒体文件"),
        Tr(L"字体文件"), Tr(L"驱动文件"), Tr(L"证书文件"), Tr(L"回收站"), Tr(L"其他情况") };
    return names[std::clamp(context, 0, ContextCount - 1)];
}

const std::vector<Command>& Catalog() { return g_catalog; }
void SetCatalog(std::vector<Command> commands) { g_catalog = std::move(commands); }

const Command* Find(const std::wstring& key)
{
    for (const Command& c : g_catalog)
        if (key == c.key)
            return &c;
    return nullptr;
}

std::vector<Entry> Defaults(int context)
{
    std::vector<Entry> out;
    for (const Default& d : DefaultsOf(context))
        out.push_back({ d.key, d.icon, d.label });
    return out;
}

std::vector<Entry> Parse(const std::wstring& text, int context)
{
    if (text.empty())
        return Defaults(context);
    std::vector<Entry> out;
    if (text == L"none")
        return out;
    for (size_t start = 0; start <= text.size();)
    {
        const size_t comma = std::min(text.find(L',', start), text.size());
        const std::wstring token = text.substr(start, comma - start);
        start = comma + 1;
        const size_t colon = token.find(L':');
        Entry e;
        e.key = token.substr(0, colon);
        const std::wstring flags = colon == std::wstring::npos ? L"i" : token.substr(colon + 1);
        e.icon = flags.find(L'i') != std::wstring::npos;
        e.label = flags.find(L'l') != std::wstring::npos;
        // Files' Resolve: entries that no longer resolve are dropped
        if (const Command* c = Find(e.key))
        {
            if (!c->glyph)
                e.icon = false;
            out.push_back(std::move(e));
        }
        if (comma >= text.size())
            break;
    }
    return out;
}

std::wstring Format(const std::vector<Entry>& entries, int context)
{
    if (entries == Defaults(context))
        return {};
    if (entries.empty())
        return L"none";
    std::wstring text;
    for (const Entry& e : entries)
    {
        if (!text.empty())
            text += L',';
        text += e.key;
        if (!IsSeparator(e.key))
            text += std::wstring(L":") + (e.icon ? L"i" : L"") + (e.label ? L"l" : L"");
    }
    return text;
}

std::vector<ButtonBar::Item> BarItems(const std::vector<Entry>& entries, int firstId, const std::function<std::wstring(const Command&)>& tip)
{
    std::vector<ButtonBar::Item> items;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        if (IsSeparator(e.key))
        {
            items.push_back({ 0 });
            continue;
        }
        const Command* c = Find(e.key);
        const bool icon = c && e.icon && c->glyph;
        if (!c || (!icon && !e.label))
            continue;
        ButtonBar::Item item;
        item.id = firstId + static_cast<int>(i);
        item.glyph = icon ? c->glyph : nullptr;
        item.label = e.label ? c->name : L"";
        item.tip = tip ? tip(*c) : c->name;
        item.dropdown = c->group;
        items.push_back(std::move(item));
    }
    return items;
}
}
