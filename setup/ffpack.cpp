// Build tool: ffpack <payload.bin> <file[=name]>... Packs the app files for the setup's RCDATA (Windows Compression API, LZMS).
#include <windows.h>
#include <compressapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include "Payload.h"

namespace
{
bool ReadAll(const wchar_t* path, std::vector<BYTE>& out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 0x7FFFFFFF &&
        (out.resize(static_cast<size_t>(size.QuadPart)), ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) &&
        read == out.size();
    CloseHandle(f);
    return ok;
}

template <class T> void Put(std::vector<BYTE>& to, const T& value)
{
    const auto* p = reinterpret_cast<const BYTE*>(&value);
    to.insert(to.end(), p, p + sizeof(T));
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        fwprintf(stderr, L"usage: ffpack <out.bin> <file[=name]>...\n");
        return 1;
    }
    std::vector<BYTE> input;
    Put(input, static_cast<uint32_t>(argc - 2));
    for (int i = 2; i < argc; ++i)
    {
        // "C:\path\THIRD-PARTY-NOTICES.txt=THIRD-PARTY-NOTICES.txt" stores the file under another name
        std::wstring arg = argv[i], name;
        if (const size_t eq = arg.find(L'=', 3); eq != std::wstring::npos)
        {
            name = arg.substr(eq + 1);
            arg.resize(eq);
        }
        else
            name = arg.substr(arg.find_last_of(L"\\/") + 1);
        std::vector<BYTE> data;
        if (!ReadAll(arg.c_str(), data))
        {
            fwprintf(stderr, L"ffpack: cannot read %s\n", arg.c_str());
            return 1;
        }
        Put(input, static_cast<uint16_t>(name.size()));
        input.insert(input.end(), reinterpret_cast<const BYTE*>(name.data()), reinterpret_cast<const BYTE*>(name.data() + name.size()));
        Put(input, static_cast<uint64_t>(data.size()));
        input.insert(input.end(), data.begin(), data.end());
    }
    COMPRESSOR_HANDLE c = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c))
        return 1;
    SIZE_T need = 0;
    Compress(c, input.data(), input.size(), nullptr, 0, &need); // ERROR_INSUFFICIENT_BUFFER: tells the size
    std::vector<BYTE> packed(need);
    SIZE_T written = 0;
    const bool ok = Compress(c, input.data(), input.size(), packed.data(), packed.size(), &written) != FALSE;
    CloseCompressor(c);
    if (!ok)
    {
        fwprintf(stderr, L"ffpack: compression failed (%lu)\n", GetLastError());
        return 1;
    }
    const PayloadHeader header{ kPayloadMagic, kPayloadVersion, input.size(), written, PayloadHash(input.data(), input.size()) };
    HANDLE f = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    DWORD a = 0, b = 0;
    const bool saved = WriteFile(f, &header, sizeof(header), &a, nullptr) && WriteFile(f, packed.data(), static_cast<DWORD>(written), &b, nullptr);
    CloseHandle(f);
    if (!saved)
        return 1;
    wprintf(L"ffpack: %d files, %zu -> %zu bytes\n", argc - 2, input.size(), static_cast<size_t>(written));
    return 0;
}
