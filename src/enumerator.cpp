#include "find4w/enumerator.hpp"
#include "find4w/filter.hpp"
#include "find4w/cli.hpp"

#include <winternl.h>

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

static void enumerate_recursive(const std::wstring& dir, const SearchConfig& config,
                                 FileFilter& filter, const FileCallback& callback, int depth) {
    if (config.max_depth >= 0 && depth > config.max_depth) return;

    filter.load_gitignore(dir);

    std::wstring nt_path = L"\\??\\" + dir;
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

    if (status != 0) return;

    alignas(8) char buffer[64 * 1024];
    bool first = true;
    std::vector<std::wstring> subdirs;

    for (;;) {
        status = NtQueryDirectoryFile(hdir, nullptr, nullptr, nullptr, &iosb,
            buffer, sizeof(buffer), FileDirectoryInformation, FALSE, nullptr, first ? TRUE : FALSE);
        first = false;

        if (status != 0) break;

        auto* entry = reinterpret_cast<FILE_DIRECTORY_INFORMATION_F4W*>(buffer);
        for (;;) {
            std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(WCHAR));

            if (name != L"." && name != L"..") {
                std::wstring full_path = dir + L"\\" + std::wstring(name);
                bool is_dir = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

                if (!filter.should_ignore(full_path, is_dir)) {
                    if (is_dir) {
                        subdirs.push_back(std::move(full_path));
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

    for (auto& sub : subdirs)
        enumerate_recursive(sub, config, filter, callback, depth + 1);
}

void enumerate_files(const SearchConfig& config, const FileCallback& callback) {
    FileFilter filter(config);
    enumerate_recursive(config.search_path, config, filter, callback, 0);
}

} // namespace f4w
