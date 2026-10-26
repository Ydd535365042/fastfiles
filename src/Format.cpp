#include "Format.h"
#include "Lang.h"
#include <atomic>
#include <cmath>

namespace
{
std::atomic<bool> g_decimal{ false };
std::atomic<int> g_date{ 0 };

// .NET's "D" (long date), "t" (short time), "g" (short date + short time) in the user's locale
std::wstring Date(const SYSTEMTIME& st, DWORD flags)
{
    wchar_t buf[128]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &st, nullptr, buf, ARRAYSIZE(buf), nullptr);
    return buf;
}
std::wstring Time(const SYSTEMTIME& st)
{
    wchar_t buf[64]{};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, ARRAYSIZE(buf));
    return buf;
}

bool Local(const FILETIME& utc, SYSTEMTIME& local)
{
    SYSTEMTIME st{};
    return FileTimeToSystemTime(&utc, &st) && SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
}

// Seconds since utc (negative: the future)
long long Elapsed(const FILETIME& utc)
{
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto a = (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const auto b = (static_cast<long long>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
    return (a - b) / 10000000;
}

std::wstring Fill(const wchar_t* pattern, long long n)
{
    std::wstring s = pattern;
    if (const size_t at = s.find(L"{0}"); at != std::wstring::npos)
        s.replace(at, 3, std::to_wstring(n));
    return s;
}
}

namespace Format
{
void SetSizeDecimal(bool decimal) { g_decimal = decimal; }
void SetDateFormat(int format) { g_date = std::clamp(format, 0, 2); }

std::wstring Size(ULONGLONG bytes)
{
    // Humanizer ByteSize: the largest unit with a value >= 1, "#.##" (trailing zeros dropped), Files' symbols
    static const wchar_t* const units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    const double base = g_decimal ? 1000.0 : 1024.0;
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= base && unit < 5)
    {
        value /= base;
        ++unit;
    }
    wchar_t num[32];
    swprintf_s(num, L"%.2f", std::floor(value * 100 + 0.5) / 100);
    std::wstring s = num;
    // Locale decimal separator
    wchar_t sep[8]{ L'.' };
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, sep, ARRAYSIZE(sep));
    while (!s.empty() && s.back() == L'0')
        s.pop_back();
    if (!s.empty() && s.back() == L'.')
        s.pop_back();
    if (const size_t dot = s.find(L'.'); dot != std::wstring::npos)
        s.replace(dot, 1, sep);
    return s + L" " + units[unit];
}

std::wstring DateShort(const FILETIME& utc)
{
    SYSTEMTIME st{};
    if ((!utc.dwHighDateTime && !utc.dwLowDateTime) || !Local(utc, st))
        return L" ";
    if (g_date == 1)
        return Date(st, DATE_SHORTDATE) + L" " + Time(st);
    if (g_date == 2)
    {
        wchar_t buf[32];
        swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }
    // ApplicationDateTimeFormatter.ToShortLabel
    const long long s = Elapsed(utc);
    if (s < 0 || s >= 7 * 86400)
        return Date(st, DATE_LONGDATE);
    if (s >= 2 * 86400) return Fill(Tr(L"{0} 天前"), s / 86400);
    if (s >= 86400) return Tr(L"1 天前");
    if (s >= 2 * 3600) return Fill(Tr(L"{0} 小时前"), s / 3600);
    if (s >= 3600) return Tr(L"1 小时前");
    if (s >= 120) return Fill(Tr(L"{0} 分钟前"), s / 60);
    if (s >= 60) return Tr(L"1 分钟前");
    if (s >= 2) return Fill(Tr(L"{0} 秒前"), s);
    if (s >= 1) return Tr(L"1 秒前");
    return Tr(L"现在");
}

std::wstring DateLong(const FILETIME& utc)
{
    SYSTEMTIME st{};
    if ((!utc.dwHighDateTime && !utc.dwLowDateTime) || !Local(utc, st))
        return L" ";
    if (g_date != 0)
        return DateShort(utc);
    // ApplicationDateTimeFormatter.ToLongLabel: "D t", plus the relative text within a week
    std::wstring text = Date(st, DATE_LONGDATE) + L" " + Time(st);
    const long long s = Elapsed(utc);
    if (s >= 0 && s < 7 * 86400)
        text += L" (" + DateShort(utc) + L")";
    return text;
}
}
