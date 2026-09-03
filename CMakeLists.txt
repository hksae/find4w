cmake_minimum_required(VERSION 3.20)
project(find4w VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC-specific optimizations
if(MSVC)
    # Release optimizations
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Oi /GL /GS- /Gy /fp:fast /DNDEBUG")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF")
    
    # Enable AVX2 (runtime detection will handle fallback)
    add_compile_options(/arch:AVX2)
    
    # Warnings
    add_compile_options(/W4 /permissive-)
    
    # UTF-8 source encoding
    add_compile_options(/utf-8)
endif()

# Sources
set(FIND4W_SOURCES
    src/main.cpp
    src/cli.cpp
    src/enumerator.cpp
    src/mft_scanner.cpp
    src/searcher.cpp
    src/simd_match.cpp
    src/thread_pool.cpp
    src/output.cpp
    src/queue.cpp
    src/filter.cpp
)

# Executable
add_executable(find4w ${FIND4W_SOURCES})

# Include directories
target_include_directories(find4w PRIVATE include)

# Link Windows libraries
target_link_libraries(find4w PRIVATE
    ntdll      # NtQueryDirectoryFile, etc.
    shlwapi    # PathMatchSpec
)

# Set subsystem to console
set_target_properties(find4w PROPERTIES
    WIN32_EXECUTABLE OFF
    OUTPUT_NAME "find4w"
)

# Install
install(TARGETS find4w RUNTIME DESTINATION bin)
