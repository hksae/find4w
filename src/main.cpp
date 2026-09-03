#include "find4w/cli.hpp"
#include "find4w/searcher.hpp"

int wmain(int argc, wchar_t* argv[]) {
    f4w::SearchConfig config;

    if (!f4w::parse_args(argc, argv, config))
        return 0;

    f4w::run_search(config);

    return 0;
}
