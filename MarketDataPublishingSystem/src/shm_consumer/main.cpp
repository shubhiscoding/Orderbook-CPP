#include <csignal>
#include <atomic>
#include <cstdlib>

#include "common/market_data.hpp"
#include "common/ring_buffer.hpp"
#include "common/shared_memory.hpp"
#include "common/cpu_affinity.hpp"
#include "common/logger.hpp"

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

int main(int argc, char* argv[]) {
    mds::log_info("===========================================");
    mds::log_info("  Shared Memory Consumer (Process B)");
    mds::log_info("===========================================");

    int cpu_core = 1;
    if (argc >= 2) {
        cpu_core = std::atoi(argv[1]);
    }

    mds::print_cpu_info();
    mds::set_cpu_affinity(cpu_core);
    mds::log_info("Configuration:");
    mds::log_info("  - CPU Core: {}", cpu_core);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        mds::log_info("Opening shared memory ring buffer...");
        mds::log_info("(Make sure the publisher is running first!)");

        using RingBuffer = mds::SPSCRingBuffer<mds::MarketData>;
        auto shm = mds::SharedMemory<RingBuffer>::open();

        mds::log_info("Successfully attached to shared memory");
        mds::log_info("Ring buffer capacity: {} messages", RingBuffer::capacity());
        mds::log_info("-------------------------------------------");
        mds::log_info("Waiting for market data...");
        mds::log_info("(Press Ctrl+C to stop)");
        mds::log_info("");

        uint64_t message_count = 0;
        uint64_t empty_polls = 0;
        mds::MarketData data;

        while (g_running) {
            if (shm->pop(data)) {
                int64_t now = mds::get_timestamp_ns();
                int64_t latency_ns = now - data.timestamp_ns;

                mds::log_market_data(data.instrument, data.bid, data.ask, data.timestamp_ns);

                message_count++;
                if (message_count % 1000 == 0) {
                    mds::log_info("  [Stats] Received {} messages, last latency: {} ns",
                                 message_count, latency_ns);
                }

                empty_polls = 0;

            } else {
                empty_polls++;

                if (empty_polls > 1000) {
#if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
#endif
                    if (empty_polls > 100000) {
                        struct timespec ts = {0, 1000};
                        nanosleep(&ts, nullptr);
                    }
                }
            }
        }

        mds::log_info("");
        mds::log_info("-------------------------------------------");
        mds::log_info("Shutting down...");
        mds::log_info("Total messages received: {}", message_count);

    } catch (const std::exception& e) {
        mds::log_error("Fatal error: {}", e.what());
        mds::log_error("Is the publisher (Process A) running?");
        return EXIT_FAILURE;
    }

    mds::log_info("SHM Consumer terminated cleanly");
    return EXIT_SUCCESS;
}
