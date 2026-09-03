#include "find4w/cli.hpp"
#include <intrin.h>
#include <io.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <filesystem>

namespace f4w {

std::string to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), sz, nullptr, nullptr);
    return result;
}

std::wstring to_utf16(const std::string& str) {
    if (str.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), sz);
    return result;
}

std::string to_lower_ascii(std::string_view str) {
    std::string result(str);
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z') c += 32;
    return result;
}

void detect_cpu_features(SearchConfig& config) {
    int cpuinfo[4];
    __cpuid(cpuinfo, 0);
    if (cpuinfo[0] >= 7) {
        __cpuidex(cpuinfo, 7, 0);
        config.has_avx2   = (cpuinfo[1] & (1 << 5)) != 0;
        // AVX-512F: EBX bit 16
        config.has_avx512 = (cpuinfo[1] & (1 << 16)) != 0;
    }
}

void detect_stdout_tty(SearchConfig& config) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    config.is_stdout_tty = GetConsoleMode(h, &mode) != 0;
    if (!config.is_stdout_tty)
        config.no_color = true;
}

void print_version() {
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), "find4w 0.2.0\n", 13, nullptr, nullptr);
}

static std::wstring process_escapes(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            switch (s[++i]) {
                case L'n':  r += L'\n'; break;
                case L't':  r += L'\t'; break;
                case L'r':  r += L'\r'; break;
                case L'\\': r += L'\\'; break;
                default:    r += L'\\'; r += s[i]; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

void print_help() {
    const char* text =
        "find4w 0.2.0 - Ultra-fast Windows-native file search\n"
        "\n"
        "USAGE:\n"
        "    find4w [OPTIONS] <PATTERN> [PATH]\n"
        "\n"
        "ARGS:\n"
        "    <PATTERN>    Search pattern (literal string)\n"
        "    [PATH]       Directory to search (default: current dir)\n"
        "\n"
        "OPTIONS:\n"
        "    -i               Case-insensitive search\n"
        "    -v               Invert match\n"
        "    -c               Count matches only\n"
        "    -q               Quiet mode (exit code only)\n"
        "    -n               Show line numbers (default: on)\n"
        "    -f <GLOB>        Search file names instead of content\n"
        "    -t <EXT>         Filter by file extension (e.g. -t cpp)\n"
        "    -A <NUM>         Show NUM lines after match\n"
        "    -B <NUM>         Show NUM lines before match\n"
        "    -C <NUM>         Show NUM lines of context\n"
        "    -j <NUM>         Number of threads (default: auto)\n"
        "    --max-depth <N>  Max directory depth\n"
        "    --max-count <N>  Stop after N matches\n"
        "    -M               Multiline search (\\n in pattern matches newline)\n"
        "    -E, --regex      Treat PATTERN as ECMAScript regular expression\n"
        "    -r, --replace    Replace matches with STRING (in-place atomic write)\n"
        "    --direct-io      Use unbuffered direct I/O (FILE_FLAG_NO_BUFFERING)\n"
        "    --no-color       Disable colored output\n"
        "    --no-ignore      Don't respect .gitignore\n"
        "    --version        Show version\n"
        "    -h, --help       Show this help\n"
        "\n"
        "NOTES:\n"
        "    Patterns with * or ? are automatically treated as glob wildcards.\n"
        "    Use -E for full ECMAScript regex syntax.\n";
    DWORD written;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), text, (DWORD)strlen(text), &written, nullptr);
}

static bool is_number(const wchar_t* s) {
    while (*s) { if (*s < L'0' || *s > L'9') return false; s++; }
    return true;
}

bool parse_args(int argc, wchar_t* argv[], SearchConfig& config) {
    bool pattern_set = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];

        if (arg == L"-h" || arg == L"--help") {
            print_help();
            return false;
        }
        if (arg == L"--version") {
            print_version();
            return false;
        }
        if (arg == L"-M" || arg == L"--multiline") {
            config.multiline = true;
        } else if (arg == L"-E" || arg == L"--regex") {
            config.use_regex = true;
        } else if ((arg == L"-r" || arg == L"--replace") && i + 1 < argc) {
            config.do_replace = true;
            config.replace_with = to_utf8(argv[++i]);
        } else if (arg == L"--direct-io") {
            config.direct_io = true;
        } else if (arg == L"-i") {
            config.case_insensitive = true;
        } else if (arg == L"-v") {
            config.invert_match = true;
        } else if (arg == L"-c") {
            config.count_only = true;
        } else if (arg == L"-q") {
            config.quiet = true;
        } else if (arg == L"-n") {
            config.show_line_numbers = true;
        } else if (arg == L"--no-color") {
            config.no_color = true;
        } else if (arg == L"--no-ignore") {
            config.no_gitignore = true;
        } else if (arg == L"-f") {
            config.files_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                config.pattern = argv[++i];
            }
        } else if (arg == L"-t" && i + 1 < argc) {
            config.file_types.push_back(to_utf8(argv[++i]));
        } else if (arg == L"-A" && i + 1 < argc) {
            config.context_after = _wtoi(argv[++i]);
        } else if (arg == L"-B" && i + 1 < argc) {
            config.context_before = _wtoi(argv[++i]);
        } else if (arg == L"-C" && i + 1 < argc) {
            int ctx = _wtoi(argv[++i]);
            config.context_before = ctx;
            config.context_after = ctx;
        } else if (arg == L"-j" && i + 1 < argc) {
            config.thread_count = _wtoi(argv[++i]);
        } else if (arg == L"--max-depth" && i + 1 < argc) {
            config.max_depth = _wtoi(argv[++i]);
        } else if (arg == L"--max-count" && i + 1 < argc) {
            config.max_results = _wtoi(argv[++i]);
        } else if (arg[0] == L'-' && arg.size() > 1) {
            auto msg = "Unknown option: " + to_utf8(std::wstring(arg)) + "\n";
            DWORD w;
            WriteConsoleA(GetStdHandle(STD_ERROR_HANDLE), msg.c_str(), (DWORD)msg.size(), &w, nullptr);
            return false;
        } else if (!pattern_set) {
            config.pattern = argv[i];
            pattern_set = true;
        } else {
            config.search_path = argv[i];
        }
    }

    if (!pattern_set && !config.files_mode) {
        print_help();
        return false;
    }

    if (config.search_path.empty()) {
        wchar_t buf[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, buf);
        config.search_path = buf;
    }

    config.search_path = std::filesystem::absolute(config.search_path).wstring();

    config.pattern = process_escapes(config.pattern);
    config.pattern_utf8 = to_utf8(config.pattern);
    config.pattern_lower_utf8 = to_lower_ascii(config.pattern_utf8);

    // Auto-detect glob: if pattern has * or ? and -E not set, convert to glob mode
    if (!config.use_regex) {
        const auto& p = config.pattern_utf8;
        if (p.find('*') != std::string::npos || p.find('?') != std::string::npos) {
            config.use_glob  = true;
            config.use_regex = true; // glob is implemented via regex
        }
    }

    if (config.thread_count <= 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        config.thread_count = static_cast<int>(si.dwNumberOfProcessors);
    }

    detect_cpu_features(config);
    detect_stdout_tty(config);

    auto root = config.search_path;
    if (root.size() >= 2 && root[1] == L':' && (root.size() == 2 || (root.size() == 3 && root[2] == L'\\'))) {
        config.use_mft = config.files_mode;
    }

    return true;
}

} // namespace f4w
