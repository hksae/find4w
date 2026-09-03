#include "find4w/output.hpp"
#include "find4w/cli.hpp"
#include <cstdio>
#include <charconv>
#include <inttypes.h>

namespace f4w {

static constexpr const char* COLOR_RESET   = "\033[0m";
static constexpr const char* COLOR_PATH    = "\033[35m";
static constexpr const char* COLOR_LINE_NO = "\033[32m";
static constexpr const char* COLOR_MATCH   = "\033[1;31m";
static constexpr const char* COLOR_STATS   = "\033[36m";

OutputWriter::OutputWriter(const SearchConfig& config)
    : config_(config)
    , stdout_handle_(GetStdHandle(STD_OUTPUT_HANDLE))
{
    buffer_.reserve(OUTPUT_BUFFER_SIZE);

    if (!config_.no_color && config_.is_stdout_tty) {
        DWORD mode;
        if (GetConsoleMode(stdout_handle_, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(stdout_handle_, mode);
        }
    }
}

OutputWriter::~OutputWriter() {
    buf_flush();
}

void OutputWriter::buf_append(const char* data, size_t len) {
    if (buffer_.size() + len > OUTPUT_BUFFER_SIZE)
        buf_flush();
    buffer_.insert(buffer_.end(), data, data + len);
}

void OutputWriter::buf_append(std::string_view sv) {
    buf_append(sv.data(), sv.size());
}

void OutputWriter::buf_flush() {
    if (buffer_.empty()) return;
    DWORD written;
    WriteFile(stdout_handle_, buffer_.data(), (DWORD)buffer_.size(), &written, nullptr);
    buffer_.clear();
}

void OutputWriter::write_colored(std::string_view text, const char* color) {
    if (config_.no_color) {
        buf_append(text);
    } else {
        buf_append(color, strlen(color));
        buf_append(text);
        buf_append(COLOR_RESET, strlen(COLOR_RESET));
    }
}

void OutputWriter::write_result(const FileResult& result) {
    if (result.matches.empty()) return;

    std::string path_utf8 = to_utf8(result.file_path);
    write_colored(path_utf8, COLOR_PATH);
    buf_append("\n", 1);

    for (const auto& m : result.matches) {
        if (config_.show_line_numbers) {
            char num_buf[16];
            auto [ptr, ec] = std::to_chars(num_buf, num_buf + sizeof(num_buf), m.line_number);
            write_colored(std::string_view(num_buf, ptr - num_buf), COLOR_LINE_NO);
            buf_append(":", 1);
        }

        if (m.col_start < m.line_content.size() && !config_.no_color) {
            buf_append(std::string_view(m.line_content.data(), m.col_start));
            uint32_t end = m.col_end < (uint32_t)m.line_content.size() ? m.col_end : (uint32_t)m.line_content.size();
            write_colored(std::string_view(m.line_content.data() + m.col_start, end - m.col_start), COLOR_MATCH);
            if (end < m.line_content.size())
                buf_append(std::string_view(m.line_content.data() + end, m.line_content.size() - end));
        } else {
            buf_append(m.line_content);
        }
        buf_append("\n", 1);
    }

    if (buffer_.size() > OUTPUT_BUFFER_SIZE / 2)
        buf_flush();
}

void OutputWriter::write_file_match(const std::wstring& path) {
    std::string p = to_utf8(path);
    write_colored(p, COLOR_PATH);
    buf_append("\n", 1);

    if (buffer_.size() > OUTPUT_BUFFER_SIZE / 2)
        buf_flush();
}

void OutputWriter::write_count(const std::wstring& path, uint64_t count) {
    std::string p = to_utf8(path);
    write_colored(p, COLOR_PATH);
    buf_append(":", 1);

    char num_buf[24];
    auto [ptr, ec] = std::to_chars(num_buf, num_buf + sizeof(num_buf), count);
    buf_append(std::string_view(num_buf, ptr - num_buf));
    buf_append("\n", 1);
}

void OutputWriter::write_replace_summary(const std::wstring& path, uint64_t count) {
    auto path_u8 = to_utf8(path);
    char cnt[32];
    int clen = snprintf(cnt, sizeof(cnt), ": %" PRIu64 " replacement(s)\n", count);

    write_colored(path_u8, COLOR_PATH);
    buf_append(cnt, clen);
}

void OutputWriter::write_stats(const SearchStats& stats, double elapsed_ms) {
    buf_flush();

    char line[256];
    int len = snprintf(line, sizeof(line),
        "\n%llu matches in %llu files (%llu searched, %.1f ms)\n",
        stats.total_matches.load(),
        stats.files_matched.load(),
        stats.files_searched.load(),
        elapsed_ms);

    if (!config_.no_color) {
        buf_append(COLOR_STATS, strlen(COLOR_STATS));
        buf_append(line, len);
        buf_append(COLOR_RESET, strlen(COLOR_RESET));
    } else {
        buf_append(line, len);
    }
    buf_flush();
}

void OutputWriter::flush() {
    buf_flush();
}

} // namespace f4w
