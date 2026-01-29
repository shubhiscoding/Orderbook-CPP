#pragma once

/**
 * logger.hpp
 *
 * Logging utilities using the fmt library.
 * Assignment requirement: NO std::cout allowed!
 *
 * Why fmt?
 * - Much faster than iostream (important for low-latency)
 * - Type-safe formatting (unlike printf)
 * - Python-like syntax: fmt::print("{} = {}", name, value)
 */

#include <fmt/core.h>
#include <fmt/chrono.h>  // For timestamp formatting
#include <ctime>
#include <chrono>

namespace mds {

/**
 * Get current timestamp in nanoseconds since epoch
 *
 * Why clock_gettime with CLOCK_REALTIME?
 * - Nanosecond precision (bonus points!)
 * - Low overhead compared to std::chrono in some cases
 * - Direct syscall, but we'll optimize this later
 */
inline int64_t get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

/**
 * Format timestamp for logging
 * Output format: [HH:MM:SS.nanoseconds]
 */
inline std::string format_timestamp(int64_t timestamp_ns) {
    int64_t seconds = timestamp_ns / 1'000'000'000LL;
    int64_t nanos = timestamp_ns % 1'000'000'000LL;

    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&time);

    return fmt::format("[{:02d}:{:02d}:{:02d}.{:09d}]",
                       tm->tm_hour, tm->tm_min, tm->tm_sec, nanos);
}

/**
 * Log a market data message in the required format:
 * [12:34:56.123456789] RELIANCE BID=2850.25 ASK=2850.75
 */
template <typename... Args>
inline void log_market_data(const char* instrument, double bid, double ask,
                            int64_t timestamp_ns) {
    fmt::print("{} {} BID={:.2f} ASK={:.2f}\n",
               format_timestamp(timestamp_ns), instrument, bid, ask);
}

/**
 * General logging function with timestamp
 */
template <typename... Args>
inline void log_info(fmt::format_string<Args...> format, Args&&... args) {
    fmt::print("[INFO] {}\n", fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
inline void log_error(fmt::format_string<Args...> format, Args&&... args) {
    fmt::print("[ERROR] {}\n", fmt::format(format, std::forward<Args>(args)...));
}

}  // namespace mds
