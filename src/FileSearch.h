#pragma once
#include "Common.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

// One file-name search (Rust ff_search) feeding a tab's results view. Rust workers hand over paths; a second worker
// turns them into PIDLs (SHParseDisplayName touches the disk, so not on the UI thread); the UI timer takes PIDLs in
// batches. Cancel() just flags the job: the threads own it through a shared_ptr and finish on their own.
class FileSearch
{
public:
    struct Progress
    {
        ULONGLONG matches = 0, files = 0, folders = 0, skipped = 0;
        bool done = false;      // walk finished and every match converted
        bool truncated = false; // stopped at the limit
        ULONGLONG elapsedMs = 0;
    };
    static constexpr ULONGLONG kLimit = 10000;

    ~FileSearch() { Cancel(); }
    void Start(const std::wstring& root, const std::wstring& query, bool includeHidden);
    // Files carrying a tag (inside root; anywhere when root is empty), from the tag database instead of a folder walk
    void StartTagged(const std::wstring& tagId, const std::wstring& root);
    void Cancel();
    bool Active() const { return m_job != nullptr; }
    // Up to max ready results (ownership moves to the caller)
    std::vector<UniquePidl> Take(size_t max);
    Progress Read() const;

private:
    struct Job
    {
        alignas(8) uint64_t progress[4]{}; // FfSearchProgress
        bool cancel = false;
        std::mutex lock;
        std::condition_variable wake;
        std::deque<std::wstring> paths;
        std::deque<UniquePidl> ready;
        std::atomic<bool> walked{ false };
        std::atomic<bool> converted{ false };
        ULONGLONG started = 0;
        std::atomic<ULONGLONG> finished{ 0 };
    };
    // The walker fills job->paths (under job->lock, notifying job->wake); a converter thread turns them into PIDLs
    void Run(std::function<void(Job&)> walker);
    std::shared_ptr<Job> m_job;
};
