#pragma once

#include "types.hpp"
#include <string>
#include <vector>

namespace f4w {

class FileFilter {
public:
    explicit FileFilter(const SearchConfig& config);

    bool should_ignore(const std::wstring& path, bool is_dir) const;
    void load_gitignore(const std::wstring& dir_path);

private:
    const SearchConfig&         config_;
    std::vector<std::string>    ignore_patterns_;
    std::vector<std::string>    file_type_exts_;

    bool matches_gitignore(const std::wstring& path, bool is_dir) const;
    bool matches_file_type(const std::wstring& path) const;
};

} // namespace f4w
