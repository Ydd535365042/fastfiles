#pragma once
#include "Common.h"

// Files' size and date text for the views FastFiles draws (the file list itself is DefView's and keeps the system's)
namespace Format
{
// Files' SizeUnitFormat: binary (1024, Files' default) or decimal (1000); ByteSize "#.##" with KB/MB/GB/TB symbols
void SetSizeDecimal(bool decimal);
std::wstring Size(ULONGLONG bytes);
// Files' DateTimeFormat: 0 应用 (relative within a week, Files' default), 1 系统 ("g"), 2 通用 (yyyy-MM-dd HH:mm:ss)
void SetDateFormat(int format);
std::wstring DateShort(const FILETIME& utc); // ToShortLabel
std::wstring DateLong(const FILETIME& utc);  // ToLongLabel
}
