#include "find4w/filter.hpp"
#include "find4w/cli.hpp"
#include <fstream>
#include <shlwapi.h>
#include <algorithm>

namespace f4w {

FileFilter::FileFilter(const SearchConfig& config) : config_(config) {
    for (const auto& ext : config.file_types) {
        std::string e = ext;
        if (!e.empty() && e[0] != '.')
            e = "." + e;
        file_type_exts_.push_back(e);
    }
}

void FileFilter::load_gitignore(const std::wstring& dir_path) {
    if (config_.no_gitignore) return;

    auto gi_path = dir_path + L"\\.gitignore";
    std::ifstream f(gi_path);
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            ignore_patterns_.push_back(line);
    }
}

bool FileFilter::should_ignore(const std::wstring& path, bool is_dir) const {
    auto name_pos = path.find_last_of(L"\\/");
    std::wstring name = (name_pos != std::wstring::npos) ? path.substr(name_pos + 1) : path;

    if (name == L"." || name == L"..") return true;
    if (name == L".git" || name == L"node_modules" || name == L".hg" || name == L".svn")
        return true;

    if (!is_dir && !file_type_exts_.empty() && !matches_file_type(path))
        return true;

    if (!config_.no_gitignore && matches_gitignore(path, is_dir))
        return true;

    return false;
}

bool FileFilter::matches_file_type(const std::wstring& path) const {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;

    std::string ext = to_utf8(path.substr(dot));
    for (auto& c : ext)
        if (c >= 'A' && c <= 'Z') c += 32;

    for (const auto& allowed : file_type_exts_) {
        if (ext == allowed) return true;
    }
    return false;
}

bool FileFilter::matches_gitignore(const std::wstring& path, bool is_dir) const {
    std::string path_utf8 = to_utf8(path);
    std::wstring wpath(path);

    for (const auto& pattern : ignore_patterns_) {
        bool negated = (!pattern.empty() && pattern[0] == '!');
        std::string pat = negated ? pattern.substr(1) : pattern;

        if (pat.back() == '/' && !is_dir) continue;
        if (pat.back() == '/') pat.pop_back();

        std::wstring wpat = to_utf16(pat);

        auto name_pos = wpath.find_last_of(L"\\/");
        std::wstring name = (name_pos != std::wstring::npos) ? wpath.substr(name_pos + 1) : wpath;

        if (PathMatchSpecW(name.c_str(), wpat.c_str())) {
            if (negated) return false;
            return true;
        }
    }
    return false;
}

} // namespace f4w
