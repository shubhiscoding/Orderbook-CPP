/**
 * Process B - Shared Memory Consumer
 *
 * This is the LOW-LATENCY path for receiving market data.
 *
 * Why SHM is faster than TCP:
 * ===========================
 *
 * TCP Path (Process C):
 *   Publisher → Kernel send buffer → Network stack → Kernel recv buffer → Consumer
 *   Latency: ~10-50 microseconds (syscalls, copying, protocol overhead)
 *
 * SHM Path (this process):
 *   Publisher → Shared Memory ← Consumer
 *   Latency: ~100-500 nanoseconds (direct memory access!)
 *
 * The ring buffer is in memory that BOTH processes can access directly.
 * No kernel involvement in the data path = ultra-low latency!
 *
 * How it works:
 * =============
 *
 *   Publisher (Process A)              Consumer (Process B - this)
 *   ────────────────────              ─────────────────────────────
 *         │                                      │
 *         │ push(data)                           │ pop(data)
 *         ▼                                      ▼
 *   ┌─────────────────────────────────────────────────┐
 *   │            Shared Memory Ring Buffer            │
 *   │  ┌───┬───┬───┬───┬───┬───┬───┬───┐            │
 *   │  │ 5 │ 6 │   │   │   │ 2 │ 3 │ 4 │            │
 *   │  └───┴───┴───┴───┴───┴───┴───┴───┘            │
 *   │        ↑                   ↑                   │
 *   │    write_idx           read_idx                │
 *   └─────────────────────────────────────────────────┘
 *
 * Memory ordering ensures:
 * - Consumer sees data AFTER producer's write_idx update
 * - Producer sees read_idx AFTER consumer finishes reading
 */

#include <csignal>    // signal handling
#include <atomic>     // atomic flag
#include <cstdlib>    // exit codes

#include "common/market_data.hpp"
#include "common/ring_buffer.hpp"
#include "common/shared_memory.hpp"
#include "common/logger.hpp"

// ============================================================================
// Global shutdown flag
// ============================================================================
std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

// ============================================================================
// Main Function
// ============================================================================

int main() {
    mds::log_info("===========================================");
    mds::log_info("  Shared Memory Consumer (Process B)");
    mds::log_info("===========================================");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // ====================================================================
        // Step 1: Open existing shared memory (created by publisher)
        // ====================================================================
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

        // ====================================================================
        // Step 2: Main consumption loop
        // ====================================================================
        uint64_t message_count = 0;
        uint64_t empty_polls = 0;
        mds::MarketData data;

        while (g_running) {
            // Try to pop data from ring buffer
            // pop() returns false if buffer is empty (non-blocking)
            if (shm->pop(data)) {
                // ============================================================
                // Successfully received data - log it!
                // ============================================================

                // Calculate latency: current time - message timestamp
                int64_t now = mds::get_timestamp_ns();
                int64_t latency_ns = now - data.timestamp_ns;

                // Log in the format specified by assignment:
                // [12:34:56.123456789] RELIANCE BID=2850.25 ASK=2850.75
                mds::log_market_data(data.instrument, data.bid, data.ask, data.timestamp_ns);

                // Also log latency periodically (every 1000 messages)
                message_count++;
                if (message_count % 1000 == 0) {
                    mds::log_info("  [Stats] Received {} messages, last latency: {} ns",
                                 message_count, latency_ns);
                }

                empty_polls = 0;  // Reset empty poll counter

            } else {
                // Buffer empty - no data available
                empty_polls++;

                // If we've polled empty many times, yield CPU briefly
                // This prevents burning 100% CPU when no data
                if (empty_polls > 1000) {
                    // Use pause instruction on x86 for efficient spin-wait
                    // This hints to CPU we're in a spin loop
#if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
#endif
                    // After many empty polls, do a tiny sleep to save CPU
                    if (empty_polls > 100000) {
                        // 1 microsecond sleep - still very responsive
                        struct timespec ts = {0, 1000};  // 0 sec, 1000 ns = 1 µs
                        nanosleep(&ts, nullptr);
                    }
                }
            }
        }

        // ====================================================================
        // Cleanup and stats
        // ====================================================================
        mds::log_info("");
        mds::log_info("-------------------------------------------");
        mds::log_info("Shutting down...");
        mds::log_info("Total messages received: {}", message_count);

        // SharedMemory destructor will unmap (but NOT unlink - only owner does that)

    } catch (const std::exception& e) {
        mds::log_error("Fatal error: {}", e.what());
        mds::log_error("Is the publisher (Process A) running?");
        return EXIT_FAILURE;
    }

    mds::log_info("SHM Consumer terminated cleanly");
    return EXIT_SUCCESS;
}
