#pragma once
#include <string>
#include <vector>

// Files' app language (GeneralPage 语言, AppLanguageHelper). FastFiles' strings are written in Chinese; Tr() returns
// them in the chosen language from res/lang.txt (tools/gen-lang.ps1: Files' own translations where Files has the
// same string, English otherwise). Missing strings fall back to English, then to the Chinese source.
// The language is read from settings.txt once, on first use; changing it takes a restart, as in Files.
namespace Lang
{
// Stable pointer: the translation, or `zh` itself. "标签\tCtrl+E" keeps the part after the tab.
const wchar_t* Tr(const wchar_t* zh);
inline std::wstring Tr(const std::wstring& zh) { return Tr(zh.c_str()); }
// Files' GetLocalizedFormatResource: Tr(pattern), then "{0}" arguments and ICU "{0, plural, one {# item} other {# items}}"
// (one = 1, as Files' English-first plural rules; "#" is the number)
std::wstring Format(const wchar_t* zhPattern, const std::vector<std::wstring>& args);

struct Language
{
    std::wstring code; // "" = 使用系统设置
    std::wstring name; // native name
};
std::vector<Language> Available(); // 使用系统设置 first, then by name
std::wstring Active();             // the language in use ("zh-Hans", "en-US", ...)
}

using Lang::Tr;
