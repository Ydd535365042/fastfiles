#pragma once
#include <string>

// Files' FileLogger (debug_<privileged|unprivileged>.log next to the settings): "time|level|message" lines,
// private names redacted like LogPathHelper, the last 100 lines kept at startup. Ctrl+. opens it.
namespace Log
{
enum class Level { Information, Warning, Error, Critical };
void Write(Level level, const std::wstring& message);
inline void Warning(const std::wstring& message) { Write(Level::Warning, message); }
inline void Error(const std::wstring& message) { Write(Level::Error, message); }
std::wstring Path();
std::wstring Sanitize(const std::wstring& message); // LogPathHelper.SanitizeMessage
void Start(); // purge to the last 100 lines, record unhandled crashes
}
