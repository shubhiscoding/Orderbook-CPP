#pragma once

#include <cstdint>
#include <cstring>

namespace mds {

constexpr size_t INSTRUMENT_NAME_SIZE = 16;

struct MarketData {
    char instrument[INSTRUMENT_NAME_SIZE];
    double bid;
    double ask;
    int64_t timestamp_ns;

    MarketData() : bid(0.0), ask(0.0), timestamp_ns(0) {
        std::memset(instrument, 0, INSTRUMENT_NAME_SIZE);
    }

    MarketData(const char* instr, double b, double a, int64_t ts)
        : bid(b), ask(a), timestamp_ns(ts) {
        std::strncpy(instrument, instr, INSTRUMENT_NAME_SIZE - 1);
        instrument[INSTRUMENT_NAME_SIZE - 1] = '\0';
    }
};

static_assert(sizeof(MarketData) == 40, "MarketData size should be 40 bytes");

}  // namespace mds
