#include "find4w/searcher.hpp"
#include "find4w/enumerator.hpp"
#include "find4w/mft_scanner.hpp"
#include "find4w/simd_match.hpp"
#include "find4w/thread_pool.hpp"
#include "find4w/output.hpp"
#include "find4w/filter.hpp"
#include "find4w/cli.hpp"
#include <shlwapi.h>
#include <thread>

static constexpr size_t TLS_BUF_INIT      = 256 * 1024;
static constexpr size_t CHUNK_SIZE        = 2 * 1024 * 1024;
static constexpr uint64_t LARGE_FILE_THR  = 100ULL * 1024 * 1024;

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

    DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return result;

    const char* needle     = config.case_insensitive ? config.pattern_lower_utf8.c_str() : config.pattern_utf8.c_str();
    size_t      needle_len = config.pattern_utf8.size();

    auto process_block = [&](const char* data, size_t data_size) {
        if (is_binary(data, data_size)) { result.is_binary = true; return; }

        // Quick prefilter: if the first needle byte doesn't appear, skip the whole block
        char first_byte = config.case_insensitive
            ? (char)tolower((unsigned char)needle[0])
            : needle[0];
        if (!memchr(data, (unsigned char)first_byte, data_size)) return;

        uint32_t line_num = result.matches.empty() ? 1 : result.matches.back().line_number + 1;
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
                    ml.col_start = (found && !config.invert_match) ? static_cast<uint32_t>(found - line_start) : 0;
                    ml.col_end   = ml.col_start + (found && !config.invert_match ? static_cast<uint32_t>(needle_len) : 0);
                    result.matches.push_back(std::move(ml));
                    result.match_count++;
                    if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results) return;
                }
            }
            line_start = line_end + 1;
            line_num++;
        }
    };

    if (file_size > LARGE_FILE_THR) {
        // Chunked reading: 2MB chunks with needle_len-1 overlap to catch cross-boundary matches
        thread_local std::vector<char> chunk_buf(CHUNK_SIZE + 4096);
        size_t overlap = needle_len > 1 ? needle_len - 1 : 0;
        size_t offset = 0;

        while (offset < file_size && !result.is_binary) {
            size_t to_read = std::min(CHUNK_SIZE, (size_t)(file_size - offset));
            DWORD read = 0;
            if (!ReadFile(hFile, chunk_buf.data() + overlap, (DWORD)to_read, &read, nullptr) || read == 0) break;
            size_t block_size = overlap + read;
            process_block(chunk_buf.data(), block_size);
            if (result.is_binary) break;
            // Copy tail to front for overlap
            if (read == to_read && overlap > 0)
                memcpy(chunk_buf.data(), chunk_buf.data() + block_size - overlap, overlap);
            else
                overlap = 0;
            offset += read;
        }
    } else if (file_size > MMAP_THRESHOLD) {
        HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) {
            auto data = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
            if (data) {
                WIN32_MEMORY_RANGE_ENTRY range{(PVOID)data, (SIZE_T)file_size};
                PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
                process_block(data, static_cast<size_t>(file_size));
                UnmapViewOfFile(data);
            }
            CloseHandle(hMap);
        }
    } else if (file_size > 0) {
        thread_local std::vector<char> tls_buf(TLS_BUF_INIT);
        size_t sz = static_cast<size_t>(file_size);
        if (tls_buf.size() < sz) tls_buf.resize(sz);
        DWORD read = 0;
        if (ReadFile(hFile, tls_buf.data(), (DWORD)sz, &read, nullptr) && read > 0)
            process_block(tls_buf.data(), read);
    }

    CloseHandle(hFile);
    return result;
}

void run_search(SearchConfig& config) {
    LARGE_INTEGER freq, start, stop;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    SearchStats stats;
    OutputWriter output(config);

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
        ConcurrentQueue<FileResult> result_queue(8192);
        std::atomic<bool> search_done{false};
        std::atomic<int>  pending{0};

        // Dedicated output thread — drains result_queue with no contention
        std::thread output_thread([&]() {
            for (;;) {
                auto item = result_queue.try_pop();
                if (!item) {
                    if (search_done.load(std::memory_order_acquire) && result_queue.empty()) break;
                    _mm_pause();
                    continue;
                }
                if (!config.quiet) {
                    if (config.count_only)
                        output.write_count(item->file_path, item->match_count);
                    else
                        output.write_result(*item);
                }
            }
        });

        ThreadPool pool(config.thread_count);

        enumerate_files(config, [&](FileEntry&& fe) {
            pending++;
            auto path = fe.path;
            auto size = fe.size;

            pool.submit([path, size, &config, &stats, &result_queue, &pending]() {
                stats.files_searched++;

                auto result = search_file_content(path, size, config);
                stats.bytes_searched += size;

                if (!result.matches.empty()) {
                    stats.files_matched++;
                    stats.total_matches += result.match_count;
                    while (!result_queue.try_push(std::move(result)))
                        _mm_pause();
                }

                pending--;
            });
        });

        while (pending.load(std::memory_order_acquire) > 0)
            _mm_pause();

        search_done.store(true, std::memory_order_release);
        pool.shutdown();
        output_thread.join();
    }

    output.flush();

    QueryPerformanceCounter(&stop);
    double elapsed = (double)(stop.QuadPart - start.QuadPart) / freq.QuadPart * 1000.0;

    if (!config.quiet)
        output.write_stats(stats, elapsed);
}

} // namespace f4w
