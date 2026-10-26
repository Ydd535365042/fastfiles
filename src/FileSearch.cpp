#include "FileSearch.h"
#include "Tags.h"
#include "fastfiles_core.h"
#include <condition_variable>
#include <thread>

static_assert(sizeof(FfSearchProgress) == 4 * sizeof(uint64_t), "FfSearchProgress layout");

namespace
{
const uint16_t* U16(const std::wstring& s) { return reinterpret_cast<const uint16_t*>(s.c_str()); }
}

void FileSearch::Start(const std::wstring& root, const std::wstring& query, bool includeHidden)
{
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 4u, 8u);
    // Rust does the folder walk and matching
    Run([root, query, includeHidden, threads](Job& job)
    {
        auto sink = [](void* context, const uint16_t* path, size_t len)
        {
            auto* j = static_cast<Job*>(context);
            {
                std::lock_guard guard(j->lock);
                j->paths.emplace_back(reinterpret_cast<const wchar_t*>(path), len);
            }
            j->wake.notify_one();
        };
        ff_search(U16(root), root.size(), U16(query), query.size(), includeHidden ? 1u : 0u, kLimit, threads,
            reinterpret_cast<FfSearchProgress*>(job.progress), &job.cancel, sink, &job);
    });
}

void FileSearch::StartTagged(const std::wstring& tagId, const std::wstring& root)
{
    Run([tagId, root](Job& job)
    {
        // The core first follows renamed/moved files and forgets deleted ones, then lists what carries the tag
        const auto paths = Tags::Items(tagId, root);
        std::atomic_ref<uint64_t>(job.progress[0]).store(paths.size(), std::memory_order_relaxed);
        std::atomic_ref<uint64_t>(job.progress[1]).store(paths.size(), std::memory_order_relaxed);
        {
            std::lock_guard guard(job.lock);
            for (const auto& p : paths)
                job.paths.push_back(p);
        }
        job.wake.notify_one();
    });
}

void FileSearch::Run(std::function<void(Job&)> walker)
{
    Cancel();
    auto job = std::make_shared<Job>();
    job->started = GetTickCount64();
    m_job = job;

    std::thread([job, walker = std::move(walker)]
    {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        walker(*job);
        job->walked.store(true, std::memory_order_release);
        job->wake.notify_one();
    }).detach();

    // Converter: paths -> PIDLs, in batches so the lock isn't taken per item
    std::thread([job]
    {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
        std::vector<std::wstring> batch;
        for (;;)
        {
            {
                std::unique_lock guard(job->lock);
                job->wake.wait_for(guard, std::chrono::milliseconds(50), [&] { return !job->paths.empty() || job->walked.load(); });
                if (std::atomic_ref<bool>(job->cancel).load(std::memory_order_relaxed))
                    break;
                const size_t n = std::min<size_t>(job->paths.size(), 256);
                batch.assign(std::make_move_iterator(job->paths.begin()), std::make_move_iterator(job->paths.begin() + n));
                job->paths.erase(job->paths.begin(), job->paths.begin() + n);
                if (batch.empty() && job->walked.load(std::memory_order_acquire))
                    break;
            }
            std::vector<UniquePidl> pidls;
            pidls.reserve(batch.size());
            for (const auto& path : batch)
            {
                PIDLIST_ABSOLUTE raw = nullptr;
                if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr)))
                    pidls.emplace_back(raw);
            }
            std::lock_guard guard(job->lock);
            for (auto& p : pidls)
                job->ready.push_back(std::move(p));
        }
        job->finished.store(GetTickCount64(), std::memory_order_relaxed);
        job->converted.store(true, std::memory_order_release);
        if (SUCCEEDED(co))
            CoUninitialize();
    }).detach();
}

void FileSearch::Cancel()
{
    if (m_job)
    {
        std::atomic_ref<bool>(m_job->cancel).store(true, std::memory_order_relaxed);
        m_job->wake.notify_all();
    }
    m_job.reset();
}

std::vector<UniquePidl> FileSearch::Take(size_t max)
{
    std::vector<UniquePidl> out;
    if (!m_job)
        return out;
    std::lock_guard guard(m_job->lock);
    while (!m_job->ready.empty() && out.size() < max)
    {
        out.push_back(std::move(m_job->ready.front()));
        m_job->ready.pop_front();
    }
    return out;
}

FileSearch::Progress FileSearch::Read() const
{
    Progress p;
    if (!m_job)
        return p;
    p.matches = std::atomic_ref<uint64_t>(m_job->progress[0]).load(std::memory_order_relaxed);
    p.files = std::atomic_ref<uint64_t>(m_job->progress[1]).load(std::memory_order_relaxed);
    p.folders = std::atomic_ref<uint64_t>(m_job->progress[2]).load(std::memory_order_relaxed);
    p.skipped = std::atomic_ref<uint64_t>(m_job->progress[3]).load(std::memory_order_relaxed);
    p.truncated = p.matches >= kLimit;
    if (m_job->converted.load(std::memory_order_acquire))
    {
        std::lock_guard guard(m_job->lock);
        p.done = m_job->ready.empty();
    }
    const ULONGLONG end = m_job->finished.load(std::memory_order_relaxed);
    p.elapsedMs = (end ? end : GetTickCount64()) - m_job->started;
    return p;
}
