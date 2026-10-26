#include "FileList.h"
#include "Format.h"
#include "Lang.h"
#include <map>

// Files' 分组依据 for the own list: GroupingHelper (keys and header texts), AbstractDateTimeFormatter.ToTimeSpanLabel
// (the date groups) and ShellViewModel.OrderGroups (group order; items keep the list's sort inside each group).

namespace
{
SYSTEMTIME Local(const FILETIME& utc)
{
    FILETIME local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    return st;
}

int DayNumber(const SYSTEMTIME& st) // days since 1601 (whole local days)
{
    SYSTEMTIME day = st;
    day.wHour = day.wMinute = day.wSecond = day.wMilliseconds = 0;
    FILETIME ft{};
    SystemTimeToFileTime(&day, &ft);
    return static_cast<int>((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / (10000000ULL * 86400));
}

SYSTEMTIME FromDay(int day)
{
    const ULONGLONG t = static_cast<ULONGLONG>(day) * 10000000ULL * 86400;
    FILETIME ft{ static_cast<DWORD>(t), static_cast<DWORD>(t >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    return st;
}

// Calendar.GetWeekOfYear(CalendarWeekRule.FirstFullWeek, the culture's first day of the week)
int WeekOfYear(const SYSTEMTIME& st)
{
    wchar_t first[4]{};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, first, ARRAYSIZE(first));
    const int firstDay = (_wtoi(first) + 1) % 7; // (0 Monday .. 6 Sunday) -> DayOfWeek (0 Sunday)
    SYSTEMTIME jan1 = st;
    jan1.wMonth = 1;
    jan1.wDay = 1;
    const int dayOfYear = DayNumber(st) - DayNumber(jan1);
    const int jan1Weekday = FromDay(DayNumber(jan1)).wDayOfWeek;
    const int offset = (firstDay - jan1Weekday + 7) % 7; // days before the first full week
    if (dayOfYear < offset)
    {
        SYSTEMTIME dec31 = FromDay(DayNumber(jan1) - 1);
        return WeekOfYear(dec31);
    }
    return (dayOfYear - offset) / 7 + 1;
}

std::wstring DateText(const SYSTEMTIME& st, DWORD flags)
{
    wchar_t text[128]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, flags, &st, nullptr, text, ARRAYSIZE(text), nullptr);
    return text;
}

struct TimeLabel { std::wstring text; long long index = 0; };

// AbstractDateTimeFormatter.ToTimeSpanLabel (unit: 0 year, 1 month, 2 day)
TimeLabel ToTimeSpanLabel(const FILETIME& utc, int unit)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const SYSTEMTIME time = Local(utc);
    const int today = DayNumber(now), day = DayNumber(time);
    FILETIME nowUtc{};
    GetSystemTimeAsFileTime(&nowUtc);
    const long long diffDays = (static_cast<long long>(static_cast<ULONGLONG>(nowUtc.dwHighDateTime) << 32 | nowUtc.dwLowDateTime) -
        static_cast<long long>(static_cast<ULONGLONG>(utc.dwHighDateTime) << 32 | utc.dwLowDateTime)) / (10000000LL * 86400);
    if (today < day)
        return { Tr(L"未来"), 1000000006 };
    if (today == day)
        return { Tr(L"今天"), 1000000005 };
    if (today - 1 == day)
        return { Tr(L"昨天"), 1000000004 };
    if (unit == 2)
        return { DateText(time, DATE_LONGDATE), static_cast<long long>(time.wYear) * 10000 + time.wMonth * 100 + time.wDay };
    if (diffDays <= 7 && WeekOfYear(now) == WeekOfYear(time))
        return { Tr(L"本周早些时候"), 1000000003 };
    if (diffDays <= 14 && WeekOfYear(FromDay(today - 7)) == WeekOfYear(time))
        return { Tr(L"上周"), 1000000002 };
    if (now.wYear == time.wYear && now.wMonth == time.wMonth)
        return { Tr(L"本月早些时候"), 1000000001 };
    const int lastMonthYear = now.wMonth == 1 ? now.wYear - 1 : now.wYear, lastMonth = now.wMonth == 1 ? 12 : now.wMonth - 1;
    if (lastMonthYear == time.wYear && lastMonth == time.wMonth)
        return { Tr(L"上月"), 1000000000 };
    if (unit == 1)
        return { DateText(time, DATE_YEARMONTH), static_cast<long long>(time.wYear) * 10000 + time.wMonth * 100 };
    if (now.wYear == time.wYear)
        return { Tr(L"今年早些时候"), 10000001 };
    if (now.wYear - 1 == time.wYear)
        return { Tr(L"去年"), 10000000 };
    std::wstring year = Tr(L"{0} 年");
    if (const size_t at = year.find(L"{0}"); at != std::wstring::npos)
        year.replace(at, 3, std::to_wstring(time.wYear));
    return { year, time.wYear };
}
// GroupingHelper.sizeGroups: thresholds in bytes, labels in binary units (as Files has them)
struct SizeGroup { ULONGLONG size; ULONGLONG label; };
constexpr SizeGroup kSizeGroups[] = { { 5000000000ULL, 5ULL << 30 }, { 1000000000ULL, 1ULL << 30 }, { 128000000ULL, 128ULL << 20 },
    { 1000000ULL, 1ULL << 20 }, { 16000ULL, 16ULL << 10 } };

// GetGroupSizeInfo: (key, range text, index)
void SizeInfo(ULONGLONG size, std::wstring& key, std::wstring& range, long long& index)
{
    const int n = static_cast<int>(ARRAYSIZE(kSizeGroups));
    for (int i = 0; i < n; ++i)
        if (size > kSizeGroups[i].size)
        {
            key = std::to_wstring(kSizeGroups[i].size);
            range = i > 0 ? Format::Size(kSizeGroups[i].label) + L" - " + Format::Size(kSizeGroups[i - 1].label) : Format::Size(kSizeGroups[i].label) + L" +";
            index = n - i;
            return;
        }
    key = L"0";
    range = Format::Size(0) + L" - " + Format::Size(kSizeGroups[n - 1].label);
    index = 0;
}

std::wstring Upper(std::wstring s)
{
    if (!s.empty())
        CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
}

void FileList::SetGroup(int key, int unit, bool descending)
{
    key = std::clamp(key, kGroupNone, kGroupTag);
    if (key == m_groupKey && unit == m_groupUnit && descending == m_groupDesc)
        return;
    m_groupKey = key;
    m_groupUnit = std::clamp(unit, 0, 2);
    m_groupDesc = descending;
    Rearrange();
}

// Groups m_order (already in the list's sort order) into m_groups / m_groupInfo and reorders it group by group
void FileList::Group()
{
    m_groups.clear();
    m_groupInfo.clear();
    if (m_groupKey == kGroupNone || m_order.empty())
        return;
    struct Draft
    {
        GroupInfo info;
        long long index = 0;  // SortIndexOverride
        bool folders = false; // a size group of folders (always above the files' groups)
        std::vector<int> items;
    };
    std::vector<Draft> drafts;
    std::map<std::wstring, size_t> byKey;
    for (int i : m_order)
    {
        const Item& it = m_items[static_cast<size_t>(i)];
        const bool folder = (it.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring key;
        switch (m_groupKey)
        {
        case kGroupName: key = Upper(it.display.substr(0, 1)); break;
        case kGroupSize: key = folder ? std::wstring(L"\\folder") : std::wstring(); break;
        case kGroupModified: case kGroupCreated: key = ToTimeSpanLabel(m_groupKey == kGroupModified ? it.modified : it.created, m_groupUnit).text; break;
        case kGroupType:
        {
            // Folders by their type, files by their extension (Path.GetExtension: ".dotfile" is one; none: "")
            const size_t dot = it.name.find_last_of(L'.');
            key = folder && !it.link ? L"\\" + it.type : (dot == std::wstring::npos ? std::wstring() : Lower(it.name.substr(dot)));
            break;
        }
        case kGroupTag: key = it.tags.empty() ? std::wstring(L"\\untagged") : it.tags[0]; break;
        default: break;
        }
        std::wstring range;
        long long sizeIndex = 0;
        if (m_groupKey == kGroupSize && !folder)
            SizeInfo(it.size, key, range, sizeIndex);
        auto [found, fresh] = byKey.try_emplace(key, drafts.size());
        if (fresh)
        {
            // GetGroupInfoSelector: the header from the group's first item
            Draft d;
            d.info.title = key;
            switch (m_groupKey)
            {
            case kGroupSize:
                d.folders = folder;
                d.info.title = folder ? std::wstring() : range; // (folders: their size text, empty while not calculated)
                d.index = sizeIndex;
                break;
            case kGroupModified: case kGroupCreated:
                d.index = ToTimeSpanLabel(m_groupKey == kGroupModified ? it.modified : it.created, m_groupUnit).index;
                break;
            case kGroupType:
                d.info.title = it.type;
                d.index = folder && !it.link ? 0 : 1; // Always show file sections below folders
                break;
            case kGroupTag:
            {
                d.info.countBelow = true;
                const FileTag* tag = it.tags.empty() ? nullptr : Tags::Find(m_tags, it.tags[0]);
                d.info.title = tag ? tag->name : std::wstring(Tr(L"无标签"));
                break;
            }
            default: break;
            }
            drafts.push_back(std::move(d));
        }
        drafts[found->second].items.push_back(i);
    }
    // OrderGroups: by SortIndexOverride then the text, in the group direction (size: folders first either way)
    const bool desc = m_groupDesc;
    std::stable_sort(drafts.begin(), drafts.end(), [desc](const Draft& a, const Draft& b)
        {
            if (a.folders != b.folders)
                return a.folders;
            if (a.index != b.index)
                return desc ? a.index > b.index : a.index < b.index;
            const int c = CompareStringOrdinal(a.info.title.c_str(), -1, b.info.title.c_str(), -1, FALSE) - CSTR_EQUAL;
            return desc ? c > 0 : c < 0;
        });
    m_order.clear();
    for (Draft& d : drafts)
    {
        m_groups.push_back({ static_cast<int>(m_order.size()), static_cast<int>(d.items.size()) });
        m_order.insert(m_order.end(), d.items.begin(), d.items.end());
        m_groupInfo.push_back(std::move(d.info));
    }
}
