#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The setup's embedded app files: this header, then one blob compressed with the Windows Compression API (LZMS).
// The blob (format 2): u32 file count, then per file u16 name length (UTF-16 units), the name, u64 size, the bytes.
// ffpack writes it at build time; Setup checks size and hash after decompressing.
struct PayloadHeader
{
    uint32_t magic;      // kPayloadMagic
    uint32_t version;    // format, 2
    uint64_t size;       // uncompressed bytes
    uint64_t compressed; // bytes after this header
    uint64_t hash;       // FNV-1a 64 of the uncompressed bytes
};

inline constexpr uint32_t kPayloadMagic = 0x50464646; // "FFFP"
inline constexpr uint32_t kPayloadVersion = 4; // 4: LZMS (Compression API; Windows 8 is the minimum)

inline uint64_t PayloadHash(const void* data, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t *p = static_cast<const uint8_t*>(data), *end = p + size; p < end; ++p)
        h = (h ^ *p) * 1099511628211ull;
    return h;
}

struct PayloadFile
{
    std::wstring name; // plain file name, no folders
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// Splits a decompressed blob; false when it is malformed or a name isn't a plain file name
inline bool PayloadFiles(const std::vector<uint8_t>& blob, std::vector<PayloadFile>& out)
{
    out.clear();
    size_t at = 0;
    auto take = [&](void* to, size_t n) {
        if (blob.size() - at < n)
            return false;
        memcpy(to, blob.data() + at, n);
        at += n;
        return true;
    };
    uint32_t count = 0;
    if (!take(&count, 4) || count == 0 || count > 64)
        return false;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint16_t len = 0;
        uint64_t size = 0;
        PayloadFile f;
        if (!take(&len, 2) || len == 0 || blob.size() - at < len * 2u)
            return false;
        f.name.resize(len);
        take(f.name.data(), len * 2u);
        if (f.name.find_first_of(L"\\/:") != std::wstring::npos || f.name == L"." || f.name == L"..")
            return false;
        if (!take(&size, 8) || blob.size() - at < size)
            return false;
        f.data = blob.data() + at;
        f.size = static_cast<size_t>(size);
        at += f.size;
        out.push_back(std::move(f));
    }
    return at == blob.size();
}
