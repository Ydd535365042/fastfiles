#include "FolderSize.h"
#include "Format.h"
#include "Lang.h"
#include "fastfiles_core.h"
#include <thread>

static_assert(sizeof(FfDirTotals) == 4 * sizeof(uint64_t), "FfDirTotals layout");

void FolderSizer::Start(const std::wstring& path)
{
    if (m_job && path == m_path)
        return;
    Cancel();
    if (path.empty())
        return;
    m_path = path;
    m_job = std::make_shared<Job>();
    // Enough workers to keep an SSD's queue busy; the walk is I/O bound, so this doesn't scale with cores
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 4u, 8u);
    std::thread([job = m_job, path, threads]
    {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ff_dir_size(reinterpret_cast<const uint16_t*>(path.c_str()), path.size(), threads,
            reinterpret_cast<FfDirTotals*>(job->totals), &job->cancel);
        job->done.store(true, std::memory_order_release);
    }).detach();
}

void FolderSizer::Cancel()
{
    if (m_job)
        std::atomic_ref<bool>(m_job->cancel).store(true, std::memory_order_relaxed);
    m_job.reset();
    m_path.clear();
}

FolderSizer::Snapshot FolderSizer::Read() const
{
    Snapshot s;
    if (!m_job)
        return s;
    s.done = m_job->done.load(std::memory_order_acquire);
    s.bytes = std::atomic_ref<uint64_t>(m_job->totals[0]).load(std::memory_order_relaxed);
    s.files = std::atomic_ref<uint64_t>(m_job->totals[1]).load(std::memory_order_relaxed);
    s.folders = std::atomic_ref<uint64_t>(m_job->totals[2]).load(std::memory_order_relaxed);
    s.skipped = std::atomic_ref<uint64_t>(m_job->totals[3]).load(std::memory_order_relaxed);
    return s;
}

std::wstring FormatSizeWithBytes(ULONGLONG bytes)
{

    const std::wstring human = Format::Size(bytes);
    std::wstring digits = std::to_wstring(bytes);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, L',');
    return std::wstring(human) + L" (" + digits + Tr(L" 字节)");
}
