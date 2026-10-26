#pragma once
#include "Common.h"
#include <atomic>
#include <functional>
#include <mutex>

// Files' StatusCenter model: every long-running file operation (copy, move, delete, recycle, empty the bin, compress,
// extract) runs on a worker with shared progress counters. The window polls them for the status center (button ring,
// flyout cards, taskbar) and gets kMsgDone when one finishes. Finished jobs stay as result cards until dismissed.
// Copy / move / delete use IFileOperation silently (FileOperations.cpp), like Files' ShellFileOperations: name
// conflicts are settled beforehand in Files' own dialog, access-denied and file-in-use come back as kMsgAsk questions.
class Operations
{
public:
    enum class Kind { Compress, Extract, Copy, Move, Delete, Recycle, EmptyBin };
    enum class State { Running, Done, Cancelled, Failed };
    enum class Collision { NewName, Replace, Skip }; // Files' FileNameConflictResolveOptionType
    enum class Ask { None, Elevate, InUse };         // questions a worker waits on (Files' RequestAdminOperation / FileInUseDialog)
    struct Job
    {
        unsigned id = 0;
        Kind kind = Kind::Compress;
        std::wstring title;  // "压缩 照片.zip"
        std::wstring target; // archive (compress) or folder (extract, copy, move)
        std::wstring archive; // the zip either way
        std::wstring folder; // folder that shows the result (to select it there)
        std::vector<std::wstring> sources; // copy / move / delete: the top-level items
        std::vector<UniquePidl> pidls;     // delete: the same items as the view had them (recycle bin items)
        std::wstring from;                 // delete: the folder name the card shows ("" = the first source's parent)
        std::vector<Collision> collisions; // copy / move: one per source
        size_t items = 0;    // top-level items (the card's count until the walk has counted everything)
        bool openWhenDone = false; // Files' 完成后打开目标文件夹
        alignas(8) uint64_t progress[4]{}; // total bytes, done bytes, total files, done files
        bool cancel = false;
        std::atomic<int> state{ 0 }; // State
        std::atomic<bool> counted{ false }; // copy / move / delete: the walk finished (Files' EnumerationCompleted)
        std::atomic<bool> started{ false }; // the shell reported progress at least once
        std::wstring error;
        uint64_t skipped = 0;
        ULONGLONG begun = 0, finished = 0;
        // Shared with the UI thread under `lock`
        mutable std::mutex lock;
        std::wstring current; // item being processed (Files' CurrentProcessingItemName)
        Ask ask = Ask::None;
        std::wstring askDetail; // in-use: the locked file and who holds it
        bool answer = false;
        HANDLE answered = nullptr; // set by Answer / Cancel
        ~Job() { if (answered) CloseHandle(answered); }
    };
    struct View
    {
        unsigned id;
        Kind kind;
        State state;
        std::wstring title, target, archive, folder, error, current, from;
        std::vector<std::wstring> sources;
        bool openWhenDone, cancelRequested, counted, started;
        size_t items;
        ULONGLONG totalBytes, doneBytes, totalFiles, doneFiles, skipped, elapsedMs;
        int percent; // -1 while the size isn't known yet
    };
    static constexpr UINT kMsgDone = WM_APP + 66; // wParam: job id
    static constexpr UINT kMsgAsk = WM_APP + 68;  // wParam: job id; the worker waits for Answer()

    // Compress / extract removed at the user's request: FastFiles has no built-in archive support (the shell's own
    // menu items - WinRAR, 7-Zip... - cover it). Kind::Compress / Kind::Extract stay reserved (old status-center
    // table indices, Job::archive field) but nothing constructs a job with them any more.
    // Copy / move `sources` into the folder `destination`; collisions: one per source (Skip ones are left out)
    unsigned Transfer(HWND notify, bool move, std::vector<std::wstring> sources, std::vector<Collision> collisions, std::wstring destination);
    // items: the selection's pidls (same order as sources); from: the folder name for the card ("" = from the paths)
    unsigned Delete(HWND notify, std::vector<std::wstring> sources, std::vector<UniquePidl> items, bool permanently, std::wstring from);
    unsigned EmptyBin(HWND notify);
    void Cancel(unsigned id);
    void CancelAll();
    void Answer(unsigned id, bool yes); // reply to kMsgAsk
    bool Question(unsigned id, Ask& ask, std::wstring& detail) const;
    void Forget(unsigned id); // drop a finished job from the list
    void ForgetFinished();
    std::vector<View> List() const; // newest first (Files inserts cards at the top)
    bool Find(unsigned id, View& out) const;
    int Running() const;
    bool Any() const { return !m_jobs.empty(); }

private:
    unsigned Run(HWND notify, std::shared_ptr<Job> job, std::function<void(Job&)> work);
    static View Snapshot(const Job& job);
    std::vector<std::shared_ptr<Job>> m_jobs;
    unsigned m_next = 1;
};

// FileOperations.cpp: the IFileOperation workers
namespace FileOperations
{
void Transfer(Operations::Job& job, HWND owner, bool move);
void Delete(Operations::Job& job, HWND owner, bool permanently);
void EmptyBin(Operations::Job& job);
}
