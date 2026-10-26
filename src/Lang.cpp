#include "Lang.h"
#include "Settings.h"
#include "resource.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace Lang
{
namespace
{
using Table = std::unordered_map<std::wstring, std::wstring>;

struct State
{
    std::wstring active; // "zh-Hans" = the source strings
    Table target, english;
    std::unordered_map<std::wstring, std::wstring> joined; // results that had to be built ("label\tshortcut")
    std::mutex lock;
};

std::wstring Widen(const char* s, int n)
{
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(len, 0)), L'\0');
    if (len > 0)
        MultiByteToWideChar(CP_UTF8, 0, s, n, out.data(), len);
    return out;
}

std::wstring Unescape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == L'\\' && i + 1 < s.size())
        {
            const wchar_t c = s[++i];
            out += c == L't' ? L'\t' : c == L'n' ? L'\n' : c;
        }
        else
            out += s[i];
    }
    return out;
}

// The embedded res/lang.txt: "[code]" lines, then "zh<TAB>translation" lines. A checkout with core.autocrlf
// gives it CRLF line ends; those are dropped so the "\n" lookups below work either way.
std::string Resource()
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_LANG), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const char* data = static_cast<const char*>(mem ? LockResource(mem) : nullptr);
    std::string all = data ? std::string(data, SizeofResource(nullptr, res)) : std::string();
    all.erase(std::remove(all.begin(), all.end(), '\r'), all.end());
    return all;
}

std::vector<std::wstring> Codes(const std::string& all)
{
    std::vector<std::wstring> codes;
    for (size_t at = 0; at < all.size();)
    {
        size_t end = all.find('\n', at);
        if (end == std::string::npos)
            end = all.size();
        if (end > at + 2 && all[at] == '[' && all[end - 1] == ']')
            codes.push_back(Widen(all.data() + at + 1, static_cast<int>(end - at - 2)));
        at = end + 1;
    }
    return codes;
}

void Load(const std::string& all, const std::wstring& code, Table& out)
{
    std::string ascii; // language codes are ASCII ("en-US")
    for (wchar_t c : code)
        ascii += static_cast<char>(c < 0x80 ? c : '?');
    const std::string header = "[" + ascii + "]\n";
    size_t at = all.find(header);
    if (at == std::string::npos)
        return;
    at += header.size();
    while (at < all.size() && all[at] != '[')
    {
        size_t end = all.find('\n', at);
        if (end == std::string::npos)
            end = all.size();
        const std::wstring line = Widen(all.data() + at, static_cast<int>(end - at));
        if (const size_t tab = line.find(L'\t'); tab != std::wstring::npos)
            out.emplace(Unescape(line.substr(0, tab)), Unescape(line.substr(tab + 1)));
        at = end + 1;
    }
}

// Files' AppLanguageHelper: the chosen language, else the Windows UI language; one we don't have -> en-US
std::wstring Resolve(std::wstring code, const std::vector<std::wstring>& codes)
{
    if (code.empty())
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
        if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0))
            code = name;
    }
    auto lower = [](std::wstring s) { CharLowerBuffW(s.data(), static_cast<DWORD>(s.size())); return s; };
    const std::wstring c = lower(code);
    if (c.rfind(L"zh", 0) == 0)
        return c.find(L"hant") != std::wstring::npos || c == L"zh-tw" || c == L"zh-hk" || c == L"zh-mo" ? L"zh-Hant" : L"zh-Hans";
    for (const auto& k : codes)
        if (lower(k) == c)
            return k;
    const std::wstring primary = c.substr(0, c.find(L'-'));
    for (const auto& k : codes)
        if (lower(k).substr(0, lower(k).find(L'-')) == primary)
            return k;
    return L"en-US";
}

State& Get()
{
    static State* state = []
    {
        auto* s = new State; // never freed: strings handed out stay valid for the whole run
        const std::string all = Resource();
        s->active = Resolve(LoadSettings().language, Codes(all));
        if (s->active != L"zh-Hans")
        {
            Load(all, s->active, s->target);
            if (s->active != L"en-US" && s->active != L"zh-Hant")
                Load(all, L"en-US", s->english);
        }
        return s;
    }();
    return *state;
}

const std::wstring* Find(State& s, const std::wstring& key)
{
    if (auto it = s.target.find(key); it != s.target.end())
        return &it->second;
    if (auto it = s.english.find(key); it != s.english.end())
        return &it->second;
    return nullptr;
}
}

const wchar_t* Tr(const wchar_t* zh)
{
    if (!zh || !*zh)
        return zh;
    State& s = Get();
    if (s.active == L"zh-Hans")
        return zh;
    const std::wstring key = zh;
    std::lock_guard guard(s.lock);
    if (const std::wstring* v = Find(s, key))
        return v->c_str();
    // "\t已选择 ", "解压文件\tCtrl+E": the label translated, tabs and shortcut kept
    const size_t lead = key.find_first_not_of(L'\t');
    if (lead == std::wstring::npos)
        return zh;
    const size_t tab = key.find(L'\t', lead);
    if (lead == 0 && tab == std::wstring::npos)
        return zh;
    if (auto it = s.joined.find(key); it != s.joined.end())
        return it->second.c_str();
    const std::wstring label = key.substr(lead, tab == std::wstring::npos ? std::wstring::npos : tab - lead);
    const std::wstring* v = Find(s, label);
    if (!v)
        return zh;
    std::wstring built = key.substr(0, lead) + *v + (tab == std::wstring::npos ? L"" : key.substr(tab));
    return s.joined.emplace(key, std::move(built)).first->second.c_str();
}

namespace
{
// The text of `s` from `at` up to the brace that closes the one opened just before `at`
size_t Closing(const std::wstring& s, size_t at)
{
    for (int depth = 1; at < s.size(); ++at)
        if (s[at] == L'{')
            ++depth;
        else if (s[at] == L'}' && --depth == 0)
            return at;
    return std::wstring::npos;
}

std::wstring Expand(const std::wstring& s, const std::vector<std::wstring>& args, const std::wstring* number)
{
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const wchar_t c = s[i];
        if (c == L'#' && number)
        {
            out += *number;
            continue;
        }
        if (c != L'{')
        {
            out += c;
            continue;
        }
        const size_t end = Closing(s, i + 1);
        if (end == std::wstring::npos)
        {
            out += s.substr(i);
            break;
        }
        const std::wstring inner = s.substr(i + 1, end - i - 1);
        i = end;
        const size_t comma = inner.find(L',');
        const int index = _wtoi(inner.substr(0, comma).c_str());
        const std::wstring arg = index >= 0 && index < static_cast<int>(args.size()) ? args[index] : std::wstring();
        if (comma == std::wstring::npos)
        {
            out += arg;
            continue;
        }
        // "N, plural, one {...} other {...}": the branch for the number, its "#" replaced
        const size_t kind = inner.find(L',', comma + 1);
        if (kind == std::wstring::npos)
            continue;
        const long long n = _wtoi64(arg.c_str());
        std::wstring chosen, other;
        for (size_t at = kind + 1; at < inner.size();)
        {
            const size_t open = inner.find(L'{', at);
            if (open == std::wstring::npos)
                break;
            std::wstring selector = inner.substr(at, open - at);
            selector.erase(0, selector.find_first_not_of(L' '));
            selector.erase(selector.find_last_not_of(L' ') + 1);
            const size_t close = Closing(inner, open + 1);
            if (close == std::wstring::npos)
                break;
            const std::wstring body = inner.substr(open + 1, close - open - 1);
            if (selector == L"other")
                other = body;
            else if ((selector == L"one" && n == 1) || (selector.size() > 1 && selector[0] == L'=' && _wtoi64(selector.c_str() + 1) == n))
                chosen = body;
            at = close + 1;
        }
        out += Expand(chosen.empty() ? other : chosen, args, &arg);
    }
    return out;
}
}

std::wstring Format(const wchar_t* zhPattern, const std::vector<std::wstring>& args)
{
    return Expand(Tr(zhPattern), args, nullptr);
}

std::vector<Language> Available()
{
    std::vector<Language> out;
    std::vector<std::wstring> codes = Codes(Resource());
    codes.push_back(L"zh-Hans"); // the source strings
    for (const auto& c : codes)
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH * 2]{};
        // Files shows CultureInfo.NativeName: "English (United States)", "中文(简体)"
        if (!GetLocaleInfoEx(c.c_str(), LOCALE_SNATIVEDISPLAYNAME, name, ARRAYSIZE(name)))
            wcscpy_s(name, c.c_str());
        out.push_back({ c, name });
    }
    std::sort(out.begin(), out.end(), [](const Language& a, const Language& b) { return CompareStringEx(LOCALE_NAME_INVARIANT,
        NORM_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN; });
    out.insert(out.begin(), { L"", Tr(L"使用系统设置") });
    return out;
}

std::wstring Active() { return Get().active; }
}
