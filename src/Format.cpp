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
    // 应用: the full date and time ("2026年10月27日 14:30"). Files writes "3 天前" / "5 小时前" within a week
    // (ApplicationDateTimeFormatter.ToShortLabel); the user wants the date and time itself in every row
    return Date(st, DATE_LONGDATE) + L" " + Time(st);
}

std::wstring DateLong(const FILETIME& utc)
{
    SYSTEMTIME st{};
    if ((!utc.dwHighDateTime && !utc.dwLowDateTime) || !Local(utc, st))
        return L" ";
    // ApplicationDateTimeFormatter.ToLongLabel: "D t" (without its relative "(3 天前)" suffix, as in the list)
    return DateShort(utc);
}
}
