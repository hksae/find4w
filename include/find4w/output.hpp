#pragma once

#include "types.hpp"
#include <string>
#include <vector>

namespace f4w {

class OutputWriter {
public:
    explicit OutputWriter(const SearchConfig& config);
    ~OutputWriter();

    void write_result(const FileResult& result);
    void write_file_match(const std::wstring& path);
    void write_count(const std::wstring& path, uint64_t count);
    void write_stats(const SearchStats& stats, double elapsed_ms);
    void flush();

private:
    const SearchConfig& config_;
    HANDLE              stdout_handle_;
    std::vector<char>   buffer_;

    void buf_append(const char* data, size_t len);
    void buf_append(std::string_view sv);
    void buf_flush();
    void write_colored(std::string_view text, const char* color);
};

} // namespace f4w
