#include "find4w/enumerator.hpp"
#include "find4w/filter.hpp"
#include "find4w/cli.hpp"
#include "find4w/queue.hpp"

#include <winternl.h>
#include <thread>
#include <immintrin.h>
#include <algorithm>

typedef struct _FILE_DIRECTORY_INFORMATION_F4W {
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    WCHAR         FileName[1];
} FILE_DIRECTORY_INFORMATION_F4W;

extern "C" NTSTATUS NTAPI NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry,
    PUNICODE_STRING FileName, BOOLEAN RestartScan);

#ifndef FileDirectoryInformation
#define FileDirectoryInformation ((FILE_INFORMATION_CLASS)1)
#endif

namespace f4w {

struct DirTask {
    std::wstring path;
    int depth;
};

static void process_directory(
    const DirTask& task,
    const SearchConfig& config,
    ConcurrentQueue<DirTask>& dir_queue,
    std::atomic<int>& pending,
    const FileCallback& callback)
{
    if (config.max_depth >= 0 && task.depth > config.max_depth) {
        --pending;
        return;
    }

    FileFilter filter(config);
    filter.load_gitignore(task.path);

    std::wstring nt_path = L"\\??\\" + task.path;
    UNICODE_STRING upath;
    upath.Buffer = const_cast<PWSTR>(nt_path.c_str());
    upath.Length = static_cast<USHORT>(nt_path.size() * sizeof(WCHAR));
    upath.MaximumLength = upath.Length + sizeof(WCHAR);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &upath, OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    IO_STATUS_BLOCK iosb;
    HANDLE hdir;
    NTSTATUS status = NtOpenFile(&hdir, FILE_LIST_DIRECTORY | SYNCHRONIZE,
        &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT);

    if (status != 0) {
        --pending;
        return;
    }

    alignas(8) char buffer[64 * 1024];
    bool first = true;

    for (;;) {
        status = NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb,
            buffer, sizeof(buffer), FileDirectoryInformation, FALSE, nullptr, first ? TRUE : FALSE);
        first = false;
        if (status != 0) break;

        auto* entry = reinterpret_cast<FILE_DIRECTORY_INFORMATION_F4W*>(buffer);
        for (;;) {
            std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(WCHAR));

            if (name != L"." && name != L"..") {
                std::wstring full_path = task.path + L"\\" + std::wstring(name);
                bool is_dir = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                if (!filter.should_ignore(full_path, is_dir)) {
                    if (is_dir) {
                        pending.fetch_add(1, std::memory_order_relaxed);
                        while (!dir_queue.try_push({std::move(full_path), task.depth + 1}))
                            f4w_cpu_pause();
                    } else {
                        FileEntry fe;
                        fe.path = std::move(full_path);
                        fe.size = static_cast<uint64_t>(entry->EndOfFile.QuadPart);
                        fe.is_dir = false;
                        callback(std::move(fe));
                    }
                }
            }

            if (entry->NextEntryOffset == 0) break;
            entry = reinterpret_cast<FILE_DIRECTORY_INFORMATION_F4W*>(
                reinterpret_cast<char*>(entry) + entry->NextEntryOffset);
        }
    }

    NtClose(hdir);
    pending.fetch_sub(1, std::memory_order_release);
}

void enumerate_files(const SearchConfig& config, const FileCallback& callback) {
    ConcurrentQueue<DirTask> dir_queue(65536);
    std::atomic<int> pending{1};

    dir_queue.try_push({config.search_path, 0});

    int dir_threads = std::min(config.thread_count, 8);
    std::vector<std::thread> threads;
    threads.reserve(dir_threads);

    for (int i = 0; i < dir_threads; ++i) {
        threads.emplace_back([&]() {
            for (;;) {
                std::optional<DirTask> task;
                while (!(task = dir_queue.try_pop())) {
                    if (pending.load(std::memory_order_acquire) == 0) return;
                    f4w_cpu_pause();
                }
                process_directory(*task, config, dir_queue, pending, callback);
            }
        });
    }

    for (auto& t : threads) t.join();
}

} // namespace f4w
