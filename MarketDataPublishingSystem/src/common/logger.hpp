#pragma once

#include <fmt/core.h>
#include <fmt/chrono.h>
#include <ctime>
#include <chrono>

namespace mds {

inline int64_t get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

inline std::string format_timestamp(int64_t timestamp_ns) {
    int64_t seconds = timestamp_ns / 1'000'000'000LL;
    int64_t nanos = timestamp_ns % 1'000'000'000LL;

    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm* tm = std::localtime(&time);

    return fmt::format("[{:02d}:{:02d}:{:02d}.{:09d}]",
                       tm->tm_hour, tm->tm_min, tm->tm_sec, nanos);
}

template <typename... Args>
inline void log_market_data(const char* instrument, double bid, double ask,
                            int64_t timestamp_ns) {
    fmt::print("{} {} BID={:.2f} ASK={:.2f}\n",
               format_timestamp(timestamp_ns), instrument, bid, ask);
}

template <typename... Args>
inline void log_info(fmt::format_string<Args...> format, Args&&... args) {
    fmt::print("[INFO] {}\n", fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
inline void log_error(fmt::format_string<Args...> format, Args&&... args) {
    fmt::print("[ERROR] {}\n", fmt::format(format, std::forward<Args>(args)...));
}

}  // namespace mds
