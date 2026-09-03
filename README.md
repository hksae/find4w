# find4w

![find4w](assets/find4w_header.png)

Fast Windows-native file search.

## Key optimizations


- **NtQueryDirectoryFile** — NT native API for directory traversal (~30-40% faster than FindFirstFile)
- **NTFS MFT direct read** — scans entire volume in seconds via `FSCTL_ENUM_USN_DATA`
- **SIMD pattern matching** — SSE2/AVX2 accelerated substring search
- **Memory-mapped files** — zero-copy file reading via `CreateFileMapping`
- **IOCP thread pool** — I/O Completion Ports for optimal thread scheduling
- **Lock-free queues** — minimal synchronization overhead
- **Buffered output** — direct `WriteFile` with large buffers

## Build

Requires MSVC (Visual Studio 2022+) and CMake 3.20+.

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Binary: `build/Release/find4w.exe`

## Usage

```
find4w <PATTERN> [PATH]          # search file contents
find4w -i "pattern" ./src        # case-insensitive
find4w -t cpp "TODO" .           # filter by extension
find4w -f "*.log" C:\            # search file names (MFT turbo mode)
find4w -c "error" ./logs         # count matches
find4w -M "func.*{\nreturn" .    # multiline: pattern spans lines
```

## Options

| Flag | Description |
|------|-------------|
| `-i` | Case-insensitive |
| `-v` | Invert match |
| `-c` | Count only |
| `-M` | Multiline (`\n` in pattern matches newline) |
| `-q` | Quiet (exit code only) |
| `-f <GLOB>` | Search file names |
| `-t <EXT>` | Filter by extension |
| `-A/B/C <N>` | Context lines |
| `-j <N>` | Thread count |
| `--max-depth <N>` | Max directory depth |
| `--no-color` | Disable colors |
| `--no-ignore` | Skip .gitignore |

## License

MIT
