#include "Tags.h"
#include "Theme.h"
#include "Session.h"
#include "fastfiles_core.h"
#include <cmath>

namespace
{
const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }

const std::wstring& File()
{
    static const std::wstring file = AppDataFile(L"tags.txt");
    return file;
}

std::wstring Utf8ToWide(const uint8_t* data, size_t len)
{
    if (!data || !len)
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data), static_cast<int>(len), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(data), static_cast<int>(len), out.data(), n);
    return out;
}

std::wstring Take(FfBuffer buffer)
{
    std::wstring text = Utf8ToWide(buffer.data, buffer.len);
    ff_buffer_free(buffer);
    return text;
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t sep)
{
    std::vector<std::wstring> out;
    for (size_t start = 0; start < text.size();)
    {
        size_t end = text.find(sep, start);
        if (end == std::wstring::npos)
            end = text.size();
        if (end > start)
            out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

COLORREF ParseColor(const std::wstring& text)
{
    if (text.size() != 7 || text[0] != L'#')
        return TagFallbackColor();
    const unsigned v = wcstoul(text.c_str() + 1, nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}
}

namespace Tags
{
std::vector<FileTag> List()
{
    std::vector<FileTag> out;
    for (const auto& line : Split(Take(ff_tags_list(U16(File()), File().size())), L'\n'))
    {
        const auto parts = Split(line, L'\t');
        if (parts.size() == 3)
            out.push_back({ parts[0], parts[1], ParseColor(parts[2]) });
    }
    return out;
}

const FileTag* Find(const std::vector<FileTag>& tags, const std::wstring& id)
{
    for (const auto& t : tags)
        if (t.id == id)
            return &t;
    return nullptr;
}

std::wstring ColorText(COLORREF color)
{
    wchar_t text[8];
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}

std::wstring Add(const std::wstring& name, COLORREF color)
{
    const std::wstring c = ColorText(color);
    return Take(ff_tags_add(U16(File()), File().size(), U16(name), name.size(), U16(c), c.size()));
}

bool Rename(const std::wstring& id, const std::wstring& name)
{
    return ff_tags_edit(U16(File()), File().size(), U16(id), id.size(), U16(name), name.size(), nullptr, 0);
}

bool Recolor(const std::wstring& id, COLORREF color)
{
    const std::wstring c = ColorText(color);
    return ff_tags_edit(U16(File()), File().size(), U16(id), id.size(), nullptr, 0, U16(c), c.size());
}

bool Remove(const std::wstring& id)
{
    return ff_tags_remove(U16(File()), File().size(), U16(id), id.size());
}

bool Move(size_t from, size_t to)
{
    return ff_tags_move(U16(File()), File().size(), from, to);
}

std::vector<std::wstring> Of(const std::wstring& path)
{
    if (path.empty())
        return {};
    return Split(Take(ff_tags_of(U16(File()), File().size(), U16(path), path.size())), L',');
}

bool Set(const std::vector<std::wstring>& paths, const std::wstring& id, bool on)
{
    std::wstring joined;
    for (const auto& p : paths)
        joined += p + L'\0';
    return !joined.empty() && ff_tags_set(U16(File()), File().size(), U16(joined), joined.size(), U16(id), id.size(), on);
}

std::vector<std::wstring> Items(const std::wstring& id, const std::wstring& root)
{
    return Split(Take(ff_tags_items(U16(File()), File().size(), U16(id), id.size(), U16(root), root.size())), L'\n');
}

HBITMAP MenuDot(COLORREF color, int size, bool checked)
{
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits)
        return bmp;
    auto* px = static_cast<uint32_t*>(bits);
    const double c = size / 2.0, radius = size * 0.36;
    // Tick: two strokes through (0.32,0.52) (0.45,0.65) (0.69,0.38) of the dot's box
    auto segment = [](double x, double y, double ax, double ay, double bx, double by)
    {
        const double dx = bx - ax, dy = by - ay;
        const double t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
        return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
    };
    const double stroke = std::max(1.0, size / 14.0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            // 4x4 supersampling for the edge and the tick
            double dot = 0, tick = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                {
                    const double fx = x + (sx + 0.5) / 4, fy = y + (sy + 0.5) / 4;
                    const bool in = std::hypot(fx - c, fy - c) <= radius;
                    dot += in;
                    if (checked && in)
                    {
                        const double u = fx / size, v = fy / size;
                        const double d = std::min(segment(u, v, 0.32, 0.52, 0.45, 0.65), segment(u, v, 0.45, 0.65, 0.69, 0.38)) * size;
                        tick += d <= stroke;
                    }
                }
            const double a = dot / 16, w = tick / 16;
            auto ch = [&](BYTE base) { return static_cast<uint32_t>((base * (a - w) + 255 * w) + 0.5); };
            px[y * size + x] = static_cast<uint32_t>(a * 255 + 0.5) << 24 | ch(GetRValue(color)) << 16 | ch(GetGValue(color)) << 8 | ch(GetBValue(color));
        }
    return bmp;
}

UINT ChangedMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"FastFiles.TagsChanged");
    return msg;
}

void Broadcast()
{
    const UINT msg = ChangedMessage();
    EnumWindows([](HWND w, LPARAM lp) -> BOOL
    {
        wchar_t cls[64]{};
        if (GetClassNameW(w, cls, 64) && wcscmp(cls, L"FastFiles.MainWindow") == 0)
            PostMessageW(w, static_cast<UINT>(lp), 0, 0);
        return TRUE;
    }, static_cast<LPARAM>(msg));
}
}
