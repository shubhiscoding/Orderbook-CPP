cmake_minimum_required(VERSION 3.16)
project(MarketDataPublishingSystem VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# C++ Standard Configuration
# ============================================================================
# We use C++17 as required by the assignment
# C++17 gives us: structured bindings, if-constexpr, std::optional, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Disable GNU extensions for portability

# ============================================================================
# Compiler Flags
# ============================================================================
# -Wall -Wextra: Enable most warnings to catch potential bugs
# -O3: Maximum optimization for release builds (important for low-latency)
# -march=native: Optimize for the current CPU architecture
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Default to Release build for performance
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ============================================================================
# Dependencies via FetchContent
# ============================================================================
# FetchContent allows us to download and build dependencies automatically
# This is cleaner than requiring users to install libraries manually
include(FetchContent)

# fmt library - Fast, safe formatting library (required: no std::cout)
# Why fmt? It's faster than iostream and provides Python-like formatting
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1  # Stable release
)

# nlohmann/json - Modern JSON library for C++
# Why this? Header-only, intuitive API, widely used in industry
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3  # Stable release
)

FetchContent_MakeAvailable(fmt json)

# ============================================================================
# Common Library (shared code between all processes)
# ============================================================================
# This is a header-only "library" containing:
# - Market data structures
# - Ring buffer implementation
# - Logger utilities
add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(common INTERFACE fmt::fmt nlohmann_json::nlohmann_json)

# ============================================================================
# Executable: Publisher (Process A)
# ============================================================================
# The publisher generates market data and sends it via:
# 1. TCP socket (to Process C)
# 2. Shared memory ring buffer (to Process B)
add_executable(publisher src/publisher/main.cpp)
target_link_libraries(publisher PRIVATE
    common
    pthread  # POSIX threads
    rt       # POSIX real-time extensions (for shm_open, clock_gettime)
)

# ============================================================================
# Executable: SHM Consumer (Process B)
# ============================================================================
# Reads market data from shared memory ring buffer
add_executable(shm_consumer src/shm_consumer/main.cpp)
target_link_libraries(shm_consumer PRIVATE
    common
    pthread
    rt
)

# ============================================================================
# Executable: TCP Consumer (Process C)
# ============================================================================
# Reads market data from TCP socket
add_executable(tcp_consumer src/tcp_consumer/main.cpp)
target_link_libraries(tcp_consumer PRIVATE
    common
    pthread
    rt
)

# ============================================================================
# Installation (optional)
# ============================================================================
install(TARGETS publisher shm_consumer tcp_consumer
    RUNTIME DESTINATION bin
)
