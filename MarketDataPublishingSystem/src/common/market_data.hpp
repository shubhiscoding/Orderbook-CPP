#pragma once

/**
 * market_data.hpp
 *
 * Defines the market data message structure used across all processes.
 * This is the "contract" between publisher and consumers.
 *
 * Why a fixed-size struct?
 * - Predictable memory layout for shared memory
 * - No dynamic allocation in hot path (low-latency requirement)
 * - Can be directly copied into ring buffer
 */

#include <cstdint>
#include <cstring>

namespace mds {  // mds = Market Data System

// Fixed instrument name size - avoids dynamic allocation
// 16 bytes is enough for most ticker symbols (e.g., "RELIANCE", "TATASTEEL")
constexpr size_t INSTRUMENT_NAME_SIZE = 16;

/**
 * MarketData structure representing a quote update
 *
 * Memory layout is crucial for shared memory:
 * - Fixed size allows direct memcpy
 * - No pointers (they don't work across processes!)
 * - Aligned for efficient CPU access
 */
struct MarketData {
    char instrument[INSTRUMENT_NAME_SIZE];  // Stock symbol (null-terminated)
    double bid;                              // Best bid price
    double ask;                              // Best ask price
    int64_t timestamp_ns;                    // Nanosecond timestamp

    // Default constructor - zero initialize
    MarketData() : bid(0.0), ask(0.0), timestamp_ns(0) {
        std::memset(instrument, 0, INSTRUMENT_NAME_SIZE);
    }

    // Convenience constructor
    MarketData(const char* instr, double b, double a, int64_t ts)
        : bid(b), ask(a), timestamp_ns(ts) {
        std::strncpy(instrument, instr, INSTRUMENT_NAME_SIZE - 1);
        instrument[INSTRUMENT_NAME_SIZE - 1] = '\0';  // Ensure null termination
    }
};

// Verify size at compile time - useful for debugging memory layout issues
static_assert(sizeof(MarketData) == 40, "MarketData size should be 40 bytes");

}  // namespace mds
