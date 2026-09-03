#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <memory>
#include <atomic>

namespace f4w {

struct SearchConfig {
    std::wstring             pattern;
    std::string              pattern_utf8;
    std::string              pattern_lower_utf8;
    std::wstring             search_path;
    std::vector<std::string> file_types;

    bool case_insensitive  = false;
    bool invert_match      = false;
    bool count_only        = false;
    bool quiet             = false;
    bool no_color          = false;
    bool files_mode        = false;
    bool show_line_numbers = true;
    bool no_gitignore      = false;
    bool multiline         = false;

    int context_before = 0;
    int context_after  = 0;
    int max_depth      = -1;
    int max_results    = -1;
    int thread_count   = 0;

    bool use_mft       = false;
    bool has_avx2      = false;
    bool is_stdout_tty = true;
};

struct FileEntry {
    std::wstring path;
    uint64_t     size   = 0;
    bool         is_dir = false;
};

struct MatchLine {
    uint32_t    line_number;
    uint32_t    col_start;
    uint32_t    col_end;
    std::string line_content;
};

struct FileResult {
    std::wstring            file_path;
    std::vector<MatchLine>  matches;
    uint64_t                match_count = 0;
    bool                    is_binary   = false;
};

struct SearchStats {
    std::atomic<uint64_t> files_searched{0};
    std::atomic<uint64_t> files_matched{0};
    std::atomic<uint64_t> files_skipped{0};
    std::atomic<uint64_t> total_matches{0};
    std::atomic<uint64_t> bytes_searched{0};
};

constexpr size_t READ_BUFFER_SIZE   = 64 * 1024;
constexpr size_t OUTPUT_BUFFER_SIZE = 128 * 1024;
constexpr size_t MMAP_THRESHOLD     = 32 * 1024;
constexpr size_t BINARY_CHECK_SIZE  = 8192;
constexpr size_t MAX_LINE_LENGTH    = 4096;
constexpr size_t QUEUE_BATCH_SIZE   = 64;

} // namespace f4w
