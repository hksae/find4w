#include "find4w/searcher.hpp"
#include "find4w/enumerator.hpp"
#include "find4w/mft_scanner.hpp"
#include "find4w/simd_match.hpp"
#include "find4w/thread_pool.hpp"
#include "find4w/output.hpp"
#include "find4w/filter.hpp"
#include "find4w/cli.hpp"
#include <shlwapi.h>
#include <mutex>

namespace f4w {

static bool is_binary(const char* data, size_t len) {
    size_t check = len < BINARY_CHECK_SIZE ? len : BINARY_CHECK_SIZE;
    for (size_t i = 0; i < check; ++i) {
        if (data[i] == '\0') return true;
    }
    return false;
}

static FileResult search_file_content(const std::wstring& path, uint64_t file_size,
                                       const SearchConfig& config) {
    FileResult result;
    result.file_path = path;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return result;

    const char* data = nullptr;
    HANDLE hMap = nullptr;
    std::vector<char> read_buf;
    size_t data_size = 0;

    if (file_size > MMAP_THRESHOLD && file_size > 0) {
        hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) {
            data = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
            if (data) data_size = static_cast<size_t>(file_size);
        }
    }

    if (!data) {
        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li)) {
            CloseHandle(hFile);
            return result;
        }
        data_size = static_cast<size_t>(li.QuadPart);
        if (data_size == 0) {
            CloseHandle(hFile);
            return result;
        }
        read_buf.resize(data_size);
        DWORD read;
        if (!ReadFile(hFile, read_buf.data(), (DWORD)data_size, &read, nullptr)) {
            CloseHandle(hFile);
            return result;
        }
        data = read_buf.data();
        data_size = read;
    }

    if (is_binary(data, data_size)) {
        result.is_binary = true;
        goto cleanup;
    }

    {
        const char* needle = config.case_insensitive ? config.pattern_lower_utf8.c_str() : config.pattern_utf8.c_str();
        size_t needle_len = config.pattern_utf8.size();

        uint32_t line_num = 1;
        const char* line_start = data;
        const char* end = data + data_size;

        while (line_start < end) {
            const char* line_end = line_start;
            while (line_end < end && *line_end != '\n') ++line_end;

            size_t line_len = line_end - line_start;
            if (line_len > 0 && *(line_end - 1) == '\r') --line_len;

            if (line_len <= MAX_LINE_LENGTH) {
                const char* found = simd_find(line_start, line_len, needle, needle_len,
                                              config.case_insensitive, config.has_avx2);
                bool has_match = (found != nullptr);
                if (config.invert_match) has_match = !has_match;

                if (has_match) {
                    MatchLine ml;
                    ml.line_number = line_num;
                    ml.line_content = std::string(line_start, line_len);

                    if (found && !config.invert_match) {
                        ml.col_start = static_cast<uint32_t>(found - line_start);
                        ml.col_end = ml.col_start + static_cast<uint32_t>(needle_len);
                    } else {
                        ml.col_start = 0;
                        ml.col_end = 0;
                    }

                    result.matches.push_back(std::move(ml));
                    result.match_count++;

                    if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results)
                        break;
                }
            }

            line_start = line_end + 1;
            line_num++;
        }
    }

cleanup:
    if (data && hMap) UnmapViewOfFile(data);
    if (hMap) CloseHandle(hMap);
    CloseHandle(hFile);
    return result;
}

void run_search(SearchConfig& config) {
    LARGE_INTEGER freq, start, stop;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    SearchStats stats;
    OutputWriter output(config);
    std::mutex output_mutex;

    if (config.files_mode && config.use_mft) {
        wchar_t drive = config.search_path[0];
        mft_scan_volume(drive, config, [&](FileEntry&& fe) {
            if (!config.quiet)
                output.write_file_match(fe.path);
            stats.files_matched++;
            stats.total_matches++;
        });
    } else if (config.files_mode) {
        enumerate_files(config, [&](FileEntry&& fe) {
            std::wstring name_lower = fe.path;
            for (auto& c : name_lower)
                if (c >= L'A' && c <= L'Z') c += 32;

            std::wstring pattern_lower = config.pattern;
            for (auto& c : pattern_lower)
                if (c >= L'A' && c <= L'Z') c += 32;

            bool match;
            if (config.pattern.find(L'*') != std::wstring::npos || config.pattern.find(L'?') != std::wstring::npos) {
                auto pos = fe.path.find_last_of(L"\\/");
                std::wstring name = (pos != std::wstring::npos) ? fe.path.substr(pos + 1) : fe.path;
                match = PathMatchSpecW(name.c_str(), config.pattern.c_str());
            } else {
                match = name_lower.find(pattern_lower) != std::wstring::npos;
            }

            if (match) {
                if (!config.quiet)
                    output.write_file_match(fe.path);
                stats.files_matched++;
                stats.total_matches++;
            }
        });
    } else {
        ThreadPool pool(config.thread_count);
        std::atomic<bool> done{false};
        std::atomic<int> pending{0};

        enumerate_files(config, [&](FileEntry&& fe) {
            pending++;
            auto path = fe.path;
            auto size = fe.size;

            pool.submit([path, size, &config, &stats, &output, &output_mutex, &pending]() {
                stats.files_searched++;

                auto result = search_file_content(path, size, config);
                stats.bytes_searched += size;

                if (!result.matches.empty()) {
                    stats.files_matched++;
                    stats.total_matches += result.match_count;

                    if (!config.quiet) {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        if (config.count_only)
                            output.write_count(result.file_path, result.match_count);
                        else
                            output.write_result(result);
                    }
                }

                pending--;
            });
        });

        while (pending.load() > 0)
            Sleep(1);

        pool.shutdown();
    }

    output.flush();

    QueryPerformanceCounter(&stop);
    double elapsed = (double)(stop.QuadPart - start.QuadPart) / freq.QuadPart * 1000.0;

    if (!config.quiet)
        output.write_stats(stats, elapsed);
}

} // namespace f4w
