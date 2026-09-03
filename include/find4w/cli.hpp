#pragma once

#include "types.hpp"
#include <string>

namespace f4w {

bool parse_args(int argc, wchar_t* argv[], SearchConfig& config);
void print_help();
void print_version();
void detect_cpu_features(SearchConfig& config);
void detect_stdout_tty(SearchConfig& config);

std::string  to_utf8(const std::wstring& wstr);
std::wstring to_utf16(const std::string& str);
std::string  to_lower_ascii(std::string_view str);

} // namespace f4w
