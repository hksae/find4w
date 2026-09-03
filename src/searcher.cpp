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
#include <regex>
#include <inttypes.h>

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

// Convert glob pattern (*, ?) to ECMAScript regex string
static std::string glob_to_regex(const std::string& pat) {
    std::string r;
    r.reserve(pat.size() * 2 + 4);
    for (size_t i = 0; i < pat.size(); ++i) {
        char c = pat[i];
        switch (c) {
            case '*':  r += ".*";   break;
            case '?':  r += '.';    break;
            case '.':  r += "\\.";  break;
            case '^':  r += "\\^";  break;
            case '$':  r += "\\$";  break;
            case '(':  r += "\\(";  break;
            case ')':  r += "\\)";  break;
            case '[':  r += "\\[";  break;
            case ']':  r += "\\]";  break;
            case '{':  r += "\\{";  break;
            case '}':  r += "\\}";  break;
            case '+':  r += "\\+";  break;
            case '|':  r += "\\|";  break;
            case '\\': r += "\\\\"; break;
            default:   r += c;      break;
        }
    }
    return r;
}

// Atomic in-place replacement: write to tmpfile, then MoveFileExW (atomic on same volume)
// Returns number of substitutions made, or -1 on I/O error.
static int64_t replace_in_file(const std::wstring& path, const std::regex& rx,
                               const std::string& replacement,
                               std::regex_constants::match_flag_type rx_flags) {
    // Read entire file
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz{};
    GetFileSizeEx(hFile, &sz);
    if (sz.QuadPart > 256LL * 1024 * 1024) { CloseHandle(hFile); return 0; } // skip >256MB

    std::string content((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    if (!ReadFile(hFile, content.data(), (DWORD)sz.QuadPart, &read, nullptr)) {
        CloseHandle(hFile); return -1;
    }
    CloseHandle(hFile);
    content.resize(read);

    // Count matches before replacement
    auto match_begin = std::sregex_iterator(content.begin(), content.end(), rx, rx_flags);
    int64_t n = (int64_t)std::distance(match_begin, std::sregex_iterator());
    if (n == 0) return 0;

    // Produce replaced content
    std::string replaced = std::regex_replace(content, rx, replacement, rx_flags);

    // Write to temp file adjacent to original
    std::wstring tmp_path = path + L".f4w_tmp";
    HANDLE hTmp = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTmp == INVALID_HANDLE_VALUE) return -1;
    DWORD written = 0;
    if (!WriteFile(hTmp, replaced.data(), (DWORD)replaced.size(), &written, nullptr) ||
        written != (DWORD)replaced.size()) {
        CloseHandle(hTmp);
        DeleteFileW(tmp_path.c_str());
        return -1;
    }
    CloseHandle(hTmp);

    // Atomic rename (same volume = single metadata operation on NTFS)
    if (!MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp_path.c_str());
        return -1;
    }
    return n;
}

static FileResult search_file_content(const std::wstring& path, uint64_t file_size,
                                       const SearchConfig& config) {
    FileResult result;
    result.file_path = path;

    DWORD flags = FILE_FLAG_SEQUENTIAL_SCAN;
    if (config.direct_io)
        flags |= FILE_FLAG_NO_BUFFERING;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return result;

    const char* needle     = config.case_insensitive ? config.pattern_lower_utf8.c_str() : config.pattern_utf8.c_str();
    size_t      needle_len = config.pattern_utf8.size();

    char first_byte = config.case_insensitive
        ? (char)tolower((unsigned char)needle[0]) : needle[0];

    // search_block: two-pass. Returns number of '\n' in block (for line tracking across chunks).
    auto search_block = [&](const char* data, size_t data_size, uint32_t base_line) -> uint32_t {
        if (is_binary(data, data_size)) { result.is_binary = true; return 0; }
        if (!memchr(data, (unsigned char)first_byte, data_size)) {
            return (uint32_t)std::count(data, data + data_size, '\n');
        }

        // Phase 1: SIMD newline scan → line start offset table
        thread_local std::vector<uint32_t> lt;
        lt.clear();
        lt.push_back(0);
        {
            size_t i = 0;
#if defined(__AVX512F__) && defined(__AVX512BW__)
            if (config.has_avx512) {
                __m512i nl512 = _mm512_set1_epi8('\n');
                for (; i + 64 <= data_size; i += 64) {
                    uint64_t mask = _mm512_cmpeq_epi8_mask(
                        _mm512_loadu_si512((const __m512i*)(data + i)), nl512);
                    while (mask) {
                        uint32_t b = (uint32_t)_tzcnt_u64(mask);
                        lt.push_back((uint32_t)(i + b + 1));
                        mask &= mask - 1;
                    }
                }
            } else
#endif
            if (config.has_avx2) {
                __m256i nl = _mm256_set1_epi8('\n');
                for (; i + 32 <= data_size; i += 32) {
                    int mask = _mm256_movemask_epi8(
                        _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)(data + i)), nl));
                    while (mask) {
                        uint32_t b = _tzcnt_u32((uint32_t)mask);
                        lt.push_back((uint32_t)(i + b + 1));
                        mask &= mask - 1;
                    }
                }
                _mm256_zeroupper();
            } else {
                __m128i nl = _mm_set1_epi8('\n');
                for (; i + 16 <= data_size; i += 16) {
                    int mask = _mm_movemask_epi8(
                        _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)(data + i)), nl));
                    while (mask) {
                        uint32_t b = _tzcnt_u32((uint32_t)mask);
                        lt.push_back((uint32_t)(i + b + 1));
                        mask &= mask - 1;
                    }
                }
            }
            for (; i < data_size; ++i)
                if (data[i] == '\n') lt.push_back((uint32_t)(i + 1));
        }
        uint32_t nl_count = (uint32_t)(lt.size() - 1);

        // Regex path (ECMAScript) — also used for glob patterns
        if (config.use_regex) {
            using flag_t = std::regex_constants::syntax_option_type;
            flag_t rf = std::regex_constants::ECMAScript | std::regex_constants::optimize;
            if (config.case_insensitive) rf |= std::regex_constants::icase;
            thread_local std::string  tl_pattern;
            thread_local std::regex   tl_rx;
            thread_local bool         tl_compiled = false;
            // For glob: convert * and ? to regex equivalents
            std::string effective_pattern = config.use_glob
                ? glob_to_regex(config.pattern_utf8)
                : config.pattern_utf8;
            if (!tl_compiled || tl_pattern != effective_pattern) {
                tl_pattern  = effective_pattern;
                tl_rx       = std::regex(tl_pattern, rf);
                tl_compiled = true;
            }
            std::string view(data, data_size);
            std::regex_constants::match_flag_type rx_flags = std::regex_constants::match_default;
            if (config.multiline)
                rx_flags = static_cast<std::regex_constants::match_flag_type>(
                    rx_flags | std::regex_constants::multiline);
            std::sregex_iterator it(view.begin(), view.end(), tl_rx, rx_flags);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                const auto& m = *it;
                size_t mo = (size_t)m.position();
                auto bit = std::upper_bound(lt.begin(), lt.end(), (uint32_t)mo);
                --bit;
                uint32_t ls       = *bit;
                uint32_t line_num = base_line + (uint32_t)(bit - lt.begin());
                // find line end
                const char* le = data + mo + m.length();
                while (le < data + data_size && *le != '\n') ++le;
                size_t ll = le - (data + ls);
                if (ll > 0 && *(le - 1) == '\r') --ll;
                if (ll <= MAX_LINE_LENGTH) {
                    MatchLine ml;
                    ml.line_number = line_num;
                    ml.line_content.assign(data + ls, ll);
                    ml.col_start = (uint32_t)(mo - ls);
                    ml.col_end   = ml.col_start + (uint32_t)m.length();
                    result.matches.push_back(std::move(ml));
                    result.match_count++;
                    if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results)
                        return nl_count;
                }
            }
            return nl_count;
        }

        // Multiline path: search across line boundaries
        if (config.multiline) {
            const char* p = data;
            size_t rem = data_size;
            while (rem >= needle_len) {
                const char* found = simd_find(p, rem, needle, needle_len,
                                              config.case_insensitive, config.has_avx2);
                if (!found) break;

                uint32_t mo = (uint32_t)(found - data);
                // Line number of first matched line
                auto it = std::upper_bound(lt.begin(), lt.end(), mo);
                --it;
                uint32_t ls = *it;
                uint32_t line_num = base_line + (uint32_t)(it - lt.begin());

                // Extend to end of last matched line (after pattern end)
                const char* match_end = found + needle_len;
                const char* le = match_end;
                while (le < data + data_size && *le != '\n') ++le;

                // Full content from line start to end of last matched line
                size_t content_len = le - (data + ls);
                if (content_len > 0 && *(le - 1) == '\r') --content_len;

                MatchLine ml;
                ml.line_number = line_num;
                ml.line_content.assign(data + ls, content_len);
                ml.col_start = mo - ls;
                ml.col_end   = (uint32_t)(match_end - (data + ls));
                result.matches.push_back(std::move(ml));
                result.match_count++;
                if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results) return nl_count;

                p = (le < data + data_size) ? le + 1 : le;
                rem = data_size - (p - data);
            }
            return nl_count;
        }

        if (config.invert_match) {
            // invert match: scan line by line (rare path)
            for (size_t li = 0; li < lt.size(); ++li) {
                uint32_t ls = lt[li];
                uint32_t le = (li + 1 < lt.size()) ? lt[li + 1] - 1 : (uint32_t)data_size;
                size_t ll = le - ls;
                if (ll > 0 && data[ls + ll - 1] == '\r') --ll;
                if (ll > MAX_LINE_LENGTH) continue;
                const char* found = simd_find(data + ls, ll, needle, needle_len,
                                              config.case_insensitive, config.has_avx2);
                if (!found) {
                    MatchLine ml;
                    ml.line_number = base_line + (uint32_t)li;
                    ml.line_content.assign(data + ls, ll);
                    ml.col_start = ml.col_end = 0;
                    result.matches.push_back(std::move(ml));
                    result.match_count++;
                    if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results) return nl_count;
                }
            }
            return nl_count;
        }

        // Phase 2: SIMD pattern search, binary-search line table for line number
        const char* p = data;
        size_t rem = data_size;
        while (rem >= needle_len) {
            const char* found = simd_find(p, rem, needle, needle_len,
                                          config.case_insensitive, config.has_avx2);
            if (!found) break;

            uint32_t mo = (uint32_t)(found - data);
            auto it = std::upper_bound(lt.begin(), lt.end(), mo);
            --it;
            uint32_t ls = *it;
            uint32_t line_num = base_line + (uint32_t)(it - lt.begin());

            // Find line end (scalar — only for matching lines)
            const char* le = found;
            while (le < data + data_size && *le != '\n') ++le;
            size_t ll = le - (data + ls);
            if (ll > 0 && *(le - 1) == '\r') --ll;

            if (ll <= MAX_LINE_LENGTH) {
                MatchLine ml;
                ml.line_number = line_num;
                ml.line_content.assign(data + ls, ll);
                ml.col_start = mo - ls;
                ml.col_end   = ml.col_start + (uint32_t)needle_len;
                result.matches.push_back(std::move(ml));
                result.match_count++;
                if (config.max_results > 0 && result.match_count >= (uint64_t)config.max_results) return nl_count;
            }

            p = (le < data + data_size) ? le + 1 : le;
            rem = data_size - (p - data);
        }
        return nl_count;
    };

    if (file_size > LARGE_FILE_THR) {
        // Async double-buffer pipeline: read chunk N+1 while processing chunk N.
        // Falls back to sync when direct_io is set (alignment constraints make
        // overlapped + NO_BUFFERING complex; sync is still fast there).
        static constexpr size_t ABUF_SIZE = CHUNK_SIZE;
        size_t overlap = needle_len > 1 ? needle_len - 1 : 0;
        uint32_t cur_line = 1;

        if (!config.direct_io) {
            // Two heap buffers; ping-pong between them
            std::vector<char> buf0(ABUF_SIZE + 4096);
            std::vector<char> buf1(ABUF_SIZE + 4096);

            OVERLAPPED ov0{}, ov1{};
            ov0.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            ov1.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

            OVERLAPPED* read_ov  = &ov0;  // currently issuing read
            OVERLAPPED* proc_ov  = &ov1;  // previously issued (to wait+process)
            std::vector<char>* read_buf = &buf0;
            std::vector<char>* proc_buf = &buf1;

            uint64_t offset     = 0;
            size_t   proc_olap  = 0;
            DWORD    proc_read  = 0;
            bool     first      = true;
            bool     read_pending = false;

            auto issue_read = [&](std::vector<char>* buf, OVERLAPPED* ov, size_t olap) -> bool {
                size_t remaining = file_size - offset;
                if (remaining == 0) return false;
                size_t to_read = std::min(ABUF_SIZE, remaining);
                ZeroMemory(ov, sizeof(OVERLAPPED));
                ResetEvent(ov->hEvent);
                ov->Offset     = (DWORD)(offset & 0xFFFFFFFF);
                ov->OffsetHigh = (DWORD)(offset >> 32);
                // Copy overlap from previous buffer (caller already did this)
                ReadFile(hFile, buf->data() + olap, (DWORD)to_read, nullptr, ov);
                return true;
            };

            // Issue first read (offset=0, no overlap)
            if (issue_read(read_buf, read_ov, 0)) {
                read_pending = true;
                offset += std::min(ABUF_SIZE, (size_t)file_size);
            }

            while (read_pending && !result.is_binary) {
                // Wait for the read we just issued
                WaitForSingleObject(read_ov->hEvent, INFINITE);
                DWORD bytes_read = 0;
                GetOverlappedResult(hFile, read_ov, &bytes_read, FALSE);

                size_t cur_olap  = first ? 0 : proc_olap;
                size_t block_size = cur_olap + bytes_read;
                // (overlap bytes are already in the front of read_buf from last iteration)

                // Issue next read into proc_buf while we will process read_buf
                read_pending = false;
                size_t next_olap = (bytes_read == ABUF_SIZE && overlap > 0) ? overlap : 0;
                if (offset < file_size) {
                    if (next_olap > 0)
                        memcpy(proc_buf->data(), read_buf->data() + block_size - next_olap, next_olap);
                    proc_ov->Offset     = (DWORD)(offset & 0xFFFFFFFF);
                    proc_ov->OffsetHigh = (DWORD)(offset >> 32);
                    ZeroMemory(proc_ov, sizeof(OVERLAPPED));
                    ResetEvent(proc_ov->hEvent);
                    proc_ov->Offset     = (DWORD)(offset & 0xFFFFFFFF);
                    proc_ov->OffsetHigh = (DWORD)(offset >> 32);
                    size_t to_read2 = std::min(ABUF_SIZE, (size_t)(file_size - offset));
                    ReadFile(hFile, proc_buf->data() + next_olap, (DWORD)to_read2, nullptr, proc_ov);
                    offset += to_read2;
                    read_pending = true;
                }

                // Process current block (CPU work overlaps with next I/O)
                uint32_t nls = search_block(read_buf->data(), block_size, cur_line);
                cur_line += nls;
                first = false;

                // Swap buffers
                std::swap(read_buf,  proc_buf);
                std::swap(read_ov,   proc_ov);
                proc_olap = next_olap;
            }

            CloseHandle(ov0.hEvent);
            CloseHandle(ov1.hEvent);
        } else {
            // Sync path for direct_io (sector-aligned reads)
            // Sector size alignment: buffers must be 4096-aligned
            constexpr size_t ALIGN = 4096;
            constexpr size_t ALIGNED_CHUNK = (ABUF_SIZE + ALIGN - 1) & ~(ALIGN - 1);
            static thread_local std::vector<char> dbuf(ALIGNED_CHUNK + 4096 + ALIGN);

            // Align pointer to sector boundary
            char* raw = dbuf.data();
            char* aligned = reinterpret_cast<char*>(
                (reinterpret_cast<uintptr_t>(raw) + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1));

            size_t local_olap = 0;
            size_t local_offset = 0;
            while (local_offset < file_size && !result.is_binary) {
                size_t remaining   = file_size - local_offset;
                size_t to_read     = std::min(ALIGNED_CHUNK, remaining);
                // Round up to sector for NO_BUFFERING
                size_t aligned_read = (to_read + ALIGN - 1) & ~(ALIGN - 1);
                DWORD read = 0;
                if (!ReadFile(hFile, aligned + local_olap, (DWORD)aligned_read, &read, nullptr) || read == 0) break;
                size_t actual = std::min((size_t)read, to_read);
                size_t block_size = local_olap + actual;
                search_block(aligned, block_size, cur_line);
                if (result.is_binary) break;
                cur_line += (uint32_t)std::count(aligned + local_olap, aligned + block_size, '\n');
                if (actual == to_read && local_olap > 0)
                    memcpy(aligned, aligned + block_size - local_olap, local_olap);
                else
                    local_olap = 0;
                local_offset += actual;
                if (actual == to_read && overlap > 0) local_olap = overlap;
            }
        }
    } else if (file_size > MMAP_THRESHOLD) {
        HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hMap) {
            auto data = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
            if (data) {
                WIN32_MEMORY_RANGE_ENTRY range{(PVOID)data, (SIZE_T)file_size};
                PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
                search_block(data, static_cast<size_t>(file_size), 1);
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
            search_block(tls_buf.data(), read, 1);
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
    } else if (config.do_replace) {
        // Replace mode: find & replace in-place with atomic writes
        using flag_t = std::regex_constants::syntax_option_type;
        flag_t rf = std::regex_constants::ECMAScript | std::regex_constants::optimize;
        if (config.case_insensitive) rf |= std::regex_constants::icase;

        std::string effective_pattern = config.use_glob
            ? glob_to_regex(config.pattern_utf8)
            : config.pattern_utf8;
        std::regex rx(effective_pattern, rf);

        std::regex_constants::match_flag_type rx_flags = std::regex_constants::match_default;
        if (config.multiline)
            rx_flags = static_cast<std::regex_constants::match_flag_type>(
                rx_flags | std::regex_constants::multiline);

        std::atomic<int64_t> total_subs{0};
        std::atomic<int64_t> files_changed{0};
        std::atomic<int>     pending{0};
        ThreadPool pool(config.thread_count);

        enumerate_files(config, [&](FileEntry&& fe) {
            pending++;
            auto path = fe.path;
            pool.submit([path, &rx, &rx_flags, &config, &output,
                         &stats, &total_subs, &files_changed, &pending]() {
                stats.files_searched++;
                int64_t n = replace_in_file(path, rx, config.replace_with, rx_flags);
                if (n > 0) {
                    total_subs += n;
                    files_changed++;
                    stats.files_matched++;
                    stats.total_matches += (uint64_t)n;
                    if (!config.quiet)
                        output.write_replace_summary(path, (uint64_t)n);
                }
                pending--;
            });
        });

        while (pending.load(std::memory_order_acquire) > 0) _mm_pause();
        pool.shutdown();
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
