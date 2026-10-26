#pragma once
#include "Common.h"
#include <atomic>

// Folder size on demand (Files: "calculate folder sizes"). The walk itself is the Rust core's ff_dir_size on a
// worker thread; the UI polls Snapshot() from its timer, so nothing is posted back and a stale walk just gets cancelled.
class FolderSizer
{
public:
    struct Snapshot
    {
        ULONGLONG bytes = 0, files = 0, folders = 0, skipped = 0;
        bool done = false;
    };

    ~FolderSizer() { Cancel(); }
    void Start(const std::wstring& path); // no-op if that path is already being (or was) measured
    void Cancel();
    bool Active() const { return m_job != nullptr; }
    const std::wstring& Path() const { return m_path; }
    Snapshot Read() const;

private:
    struct Job
    {
        alignas(8) uint64_t totals[4]{}; // FfDirTotals: bytes, files, folders, skipped
        bool cancel = false;
        std::atomic<bool> done{ false };
    };
    std::shared_ptr<Job> m_job;
    std::wstring m_path;
};

// "1.23 GB (1,320,443,392 字节)"
std::wstring FormatSizeWithBytes(ULONGLONG bytes);
