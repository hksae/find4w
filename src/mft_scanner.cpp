#include "find4w/mft_scanner.hpp"
#include "find4w/cli.hpp"
#include <WinIoCtl.h>
#include <shlwapi.h>
#include <unordered_map>

namespace f4w {

#pragma pack(push, 1)
struct USN_RECORD_V2 {
    DWORD         RecordLength;
    WORD          MajorVersion;
    WORD          MinorVersion;
    DWORDLONG     FileReferenceNumber;
    DWORDLONG     ParentFileReferenceNumber;
    USN           Usn;
    LARGE_INTEGER TimeStamp;
    DWORD         Reason;
    DWORD         SourceInfo;
    DWORD         SecurityId;
    DWORD         FileAttributes;
    WORD          FileNameLength;
    WORD          FileNameOffset;
    WCHAR         FileName[1];
};
#pragma pack(pop)

struct MftEntry {
    DWORDLONG    parent_ref;
    std::wstring name;
    bool         is_dir;
};

static std::wstring build_path(DWORDLONG ref,
                                std::unordered_map<DWORDLONG, MftEntry>& entries,
                                wchar_t drive) {
    std::wstring path;
    DWORDLONG current = ref;
    int safety = 512;

    while (current != 0 && safety-- > 0) {
        auto it = entries.find(current & 0x0000FFFFFFFFFFFF);
        if (it == entries.end()) break;

        if (path.empty())
            path = it->second.name;
        else
            path = it->second.name + L"\\" + path;

        DWORDLONG parent = it->second.parent_ref & 0x0000FFFFFFFFFFFF;
        if (parent == (current & 0x0000FFFFFFFFFFFF)) break;
        current = parent;
    }

    std::wstring root = {drive, L':', L'\\'};
    return root + path;
}

bool mft_scan_volume(wchar_t drive_letter, const SearchConfig& config, const MftFileCallback& callback) {
    std::wstring vol = L"\\\\.\\";
    vol += drive_letter;
    vol += L":";

    HANDLE hVol = CreateFileW(vol.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hVol == INVALID_HANDLE_VALUE) return false;

    MFT_ENUM_DATA_V0 med;
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;

    constexpr size_t BUF_SIZE = 1024 * 1024;
    auto buf = std::make_unique<BYTE[]>(BUF_SIZE);
    DWORD bytes_returned;

    std::unordered_map<DWORDLONG, MftEntry> entries;
    entries.reserve(500000);

    std::vector<DWORDLONG> file_refs;
    file_refs.reserve(100000);

    while (DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                           buf.get(), (DWORD)BUF_SIZE, &bytes_returned, nullptr)) {
        if (bytes_returned <= sizeof(USN)) break;

        auto* ptr = buf.get() + sizeof(USN);
        auto* end = buf.get() + bytes_returned;

        while (ptr < end) {
            auto* record = reinterpret_cast<USN_RECORD_V2*>(ptr);
            if (record->RecordLength == 0) break;

            std::wstring name(record->FileName, record->FileNameLength / sizeof(WCHAR));
            DWORDLONG ref = record->FileReferenceNumber & 0x0000FFFFFFFFFFFF;
            bool is_dir = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            MftEntry entry;
            entry.parent_ref = record->ParentFileReferenceNumber;
            entry.name = std::move(name);
            entry.is_dir = is_dir;
            entries[ref] = std::move(entry);

            if (!is_dir)
                file_refs.push_back(record->FileReferenceNumber);

            ptr += record->RecordLength;
        }

        med.StartFileReferenceNumber = *reinterpret_cast<DWORDLONG*>(buf.get());
    }

    CloseHandle(hVol);

    std::wstring pattern_w = config.pattern;
    for (auto& c : pattern_w)
        if (c >= L'A' && c <= L'Z') c += 32;

    for (auto ref : file_refs) {
        DWORDLONG key = ref & 0x0000FFFFFFFFFFFF;
        auto it = entries.find(key);
        if (it == entries.end()) continue;

        std::wstring name_lower = it->second.name;
        for (auto& c : name_lower)
            if (c >= L'A' && c <= L'Z') c += 32;

        bool match;
        if (config.pattern.find(L'*') != std::wstring::npos || config.pattern.find(L'?') != std::wstring::npos) {
            match = PathMatchSpecW(name_lower.c_str(), pattern_w.c_str());
        } else {
            match = name_lower.find(pattern_w) != std::wstring::npos;
        }

        if (match) {
            std::wstring path = build_path(ref, entries, drive_letter);
            FileEntry fe;
            fe.path = std::move(path);
            fe.is_dir = false;
            callback(std::move(fe));
        }
    }

    return true;
}

} // namespace f4w
