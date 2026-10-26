#include "Operations.h"
#include "Lang.h"
#include "Settings.h"
#include <thread>

unsigned Operations::Run(HWND notify, std::shared_ptr<Job> job, std::function<void(Job&)> work)
{
    job->id = m_next++;
    job->begun = GetTickCount64();
    m_jobs.push_back(job);
    std::thread([notify, job, work = std::move(work)]
    {
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, nullptr);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // IFileOperation wants an STA
        work(*job);
        if (SUCCEEDED(com))
            CoUninitialize();
        job->finished = GetTickCount64();
        PostMessageW(notify, kMsgDone, job->id, 0);
    }).detach();
    return job->id;
}

unsigned Operations::Transfer(HWND notify, bool move, std::vector<std::wstring> sources, std::vector<Collision> collisions, std::wstring destination)
{
    auto job = std::make_shared<Job>();
    job->kind = move ? Kind::Move : Kind::Copy;
    job->target = std::move(destination);
    job->items = sources.size();
    job->sources = std::move(sources);
    job->collisions = std::move(collisions);
    job->collisions.resize(job->sources.size(), Collision::NewName);
    return Run(notify, job, [notify, move](Job& j) { FileOperations::Transfer(j, notify, move); });
}

unsigned Operations::Delete(HWND notify, std::vector<std::wstring> sources, std::vector<UniquePidl> items, bool permanently, std::wstring from)
{
    auto job = std::make_shared<Job>();
    job->kind = permanently ? Kind::Delete : Kind::Recycle;
    job->items = sources.size();
    job->sources = std::move(sources);
    job->pidls = std::move(items);
    job->from = std::move(from);
    return Run(notify, job, [notify, permanently](Job& j) { FileOperations::Delete(j, notify, permanently); });
}

unsigned Operations::EmptyBin(HWND notify)
{
    auto job = std::make_shared<Job>();
    job->kind = Kind::EmptyBin;
    return Run(notify, job, [](Job& j) { FileOperations::EmptyBin(j); });
}

void Operations::Cancel(unsigned id)
{
    for (auto& j : m_jobs)
        if (j->id == id)
        {
            std::atomic_ref<bool>(j->cancel).store(true, std::memory_order_relaxed);
            std::lock_guard guard(j->lock);
            if (j->ask != Ask::None && j->answered)
            {
                j->answer = false; // a question still open is answered "no"
                SetEvent(j->answered);
            }
        }
}

void Operations::CancelAll()
{
    for (auto& j : m_jobs)
        Cancel(j->id);
}

void Operations::Answer(unsigned id, bool yes)
{
    for (auto& j : m_jobs)
        if (j->id == id)
        {
            std::lock_guard guard(j->lock);
            if (j->ask != Ask::None && j->answered)
            {
                j->answer = yes;
                SetEvent(j->answered);
            }
        }
}

bool Operations::Question(unsigned id, Ask& ask, std::wstring& detail) const
{
    for (const auto& j : m_jobs)
        if (j->id == id)
        {
            std::lock_guard guard(j->lock);
            ask = j->ask;
            detail = j->askDetail;
            return ask != Ask::None;
        }
    return false;
}

void Operations::Forget(unsigned id)
{
    std::erase_if(m_jobs, [id](const auto& j) { return j->id == id && j->state.load() != static_cast<int>(State::Running); });
}

void Operations::ForgetFinished()
{
    std::erase_if(m_jobs, [](const auto& j) { return j->state.load() != static_cast<int>(State::Running); });
}

Operations::View Operations::Snapshot(const Job& j)
{
    View v{};
    v.id = j.id;
    v.kind = j.kind;
    v.state = static_cast<State>(j.state.load(std::memory_order_acquire));
    v.title = j.title;
    v.target = j.target;
    v.archive = j.archive;
    v.folder = j.folder;
    v.openWhenDone = j.openWhenDone;
    v.sources = j.sources;
    v.from = j.from;
    v.items = j.items;
    v.cancelRequested = std::atomic_ref<bool>(const_cast<bool&>(j.cancel)).load(std::memory_order_relaxed);
    v.counted = j.counted.load(std::memory_order_acquire);
    v.started = j.started.load(std::memory_order_acquire);
    {
        std::lock_guard guard(j.lock);
        v.current = j.current;
    }
    if (v.state != State::Running)
        v.error = j.error; // written before the state, read after it
    auto read = [&j](int i) { return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(j.progress[i])).load(std::memory_order_relaxed); };
    v.totalBytes = read(0);
    v.doneBytes = read(1);
    v.totalFiles = read(2);
    v.doneFiles = read(3);
    v.skipped = v.state == State::Running ? 0 : j.skipped;
    v.elapsedMs = (j.finished ? j.finished : GetTickCount64()) - j.begun;
    v.percent = v.totalBytes ? static_cast<int>(std::min<ULONGLONG>(100, v.doneBytes * 100 / v.totalBytes))
                             : v.totalFiles ? static_cast<int>(v.doneFiles * 100 / v.totalFiles) : (v.state == State::Running ? -1 : 100);
    return v;
}

std::vector<Operations::View> Operations::List() const
{
    std::vector<View> out;
    for (auto it = m_jobs.rbegin(); it != m_jobs.rend(); ++it)
        out.push_back(Snapshot(**it));
    return out;
}

bool Operations::Find(unsigned id, View& out) const
{
    for (const auto& j : m_jobs)
        if (j->id == id)
        {
            out = Snapshot(*j);
            return true;
        }
    return false;
}

int Operations::Running() const
{
    int n = 0;
    for (const auto& j : m_jobs)
        n += j->state.load() == static_cast<int>(State::Running);
    return n;
}
