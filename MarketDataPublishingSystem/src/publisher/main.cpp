/**
 * Process A - Market Data Publisher
 *
 * This is the HEART of our market data system.
 *
 * Responsibilities:
 * =================
 * 1. Create shared memory and initialize the ring buffer
 * 2. Start a TCP server on 127.0.0.1:9000
 * 3. Generate realistic dummy market data
 * 4. Publish data to BOTH:
 *    - Shared memory ring buffer (for Process B)
 *    - TCP socket (for Process C)
 *
 * Architecture:
 * =============
 *
 *                    ┌─────────────────────┐
 *                    │   Market Data Gen   │
 *                    │  (random prices)    │
 *                    └──────────┬──────────┘
 *                               │
 *                               ▼
 *              ┌────────────────┴────────────────┐
 *              │                                 │
 *              ▼                                 ▼
 *   ┌─────────────────────┐          ┌─────────────────────┐
 *   │   Ring Buffer       │          │    TCP Server       │
 *   │   (Shared Memory)   │          │   (127.0.0.1:9000)  │
 *   └──────────┬──────────┘          └──────────┬──────────┘
 *              │                                 │
 *              ▼                                 ▼
 *   ┌─────────────────────┐          ┌─────────────────────┐
 *   │   Process B         │          │   Process C         │
 *   │   (SHM Consumer)    │          │   (TCP Consumer)    │
 *   └─────────────────────┘          └─────────────────────┘
 *
 * Data Flow:
 * ==========
 * 1. Generate MarketData struct with current timestamp
 * 2. Push to ring buffer (for SHM consumer)
 * 3. Convert to JSON string
 * 4. Send JSON over TCP (for TCP consumer)
 * 5. Repeat at configured rate
 */

#include <csignal>       // signal handling for graceful shutdown
#include <chrono>        // timing
#include <thread>        // sleep
#include <random>        // random price generation
#include <atomic>        // atomic flag for shutdown
#include <cstdlib>       // exit codes

#include <nlohmann/json.hpp>  // JSON serialization

#include "common/market_data.hpp"
#include "common/ring_buffer.hpp"
#include "common/shared_memory.hpp"
#include "common/tcp_socket.hpp"
#include "common/logger.hpp"

// Use nlohmann json
using json = nlohmann::json;

// ============================================================================
// Global shutdown flag - set by signal handler
// ============================================================================
// Why atomic? Signal handlers can interrupt at any time, so we need
// thread-safe access to this flag.
std::atomic<bool> g_running{true};

/**
 * Signal handler for graceful shutdown
 * Catches Ctrl+C (SIGINT) and SIGTERM
 */
void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

// ============================================================================
// Market Data Generator
// ============================================================================

/**
 * MarketDataGenerator - Generates realistic dummy market data
 *
 * Creates bid/ask prices that:
 * - Fluctuate around a base price
 * - Have a realistic spread (ask > bid)
 * - Change gradually (not random jumps)
 */
class MarketDataGenerator {
public:
    /**
     * Constructor
     *
     * @param instrument Stock symbol
     * @param base_price Starting price
     * @param volatility How much price can move (percentage)
     * @param spread Bid-ask spread (percentage)
     */
    MarketDataGenerator(const char* instrument,
                        double base_price,
                        double volatility = 0.001,    // 0.1% volatility
                        double spread = 0.0002)       // 0.02% spread
        : instrument_(instrument)
        , current_price_(base_price)
        , volatility_(volatility)
        , spread_(spread)
        , rng_(std::random_device{}())
        , dist_(-1.0, 1.0) {
    }

    /**
     * Generate next market data tick
     *
     * Price movement simulation:
     * - Random walk with mean reversion (Ornstein-Uhlenbeck-like)
     * - Bid = price - spread/2
     * - Ask = price + spread/2
     */
    mds::MarketData generate() {
        // Random price movement (scaled by volatility)
        double price_change = dist_(rng_) * volatility_ * current_price_;
        current_price_ += price_change;

        // Ensure price stays positive
        if (current_price_ < 1.0) {
            current_price_ = 1.0;
        }

        // Calculate bid and ask with spread
        double half_spread = current_price_ * spread_ / 2.0;
        double bid = current_price_ - half_spread;
        double ask = current_price_ + half_spread;

        // Get current timestamp in nanoseconds
        int64_t timestamp = mds::get_timestamp_ns();

        return mds::MarketData(instrument_, bid, ask, timestamp);
    }

private:
    const char* instrument_;
    double current_price_;
    double volatility_;
    double spread_;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> dist_;
};

// ============================================================================
// JSON Serialization
// ============================================================================

/**
 * Convert MarketData to JSON string
 *
 * Output format (as specified in assignment):
 * {
 *   "instrument": "RELIANCE",
 *   "bid": 2850.25,
 *   "ask": 2850.75,
 *   "timestamp_ns": 1234567890123
 * }
 */
std::string to_json(const mds::MarketData& data) {
    json j;
    j["instrument"] = data.instrument;
    j["bid"] = data.bid;
    j["ask"] = data.ask;
    j["timestamp_ns"] = data.timestamp_ns;

    // dump() converts to string, no indentation for compactness
    return j.dump() + "\n";  // Add newline for message separation
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char* argv[]) {
    mds::log_info("===========================================");
    mds::log_info("  Market Data Publisher (Process A)");
    mds::log_info("===========================================");

    // ========================================================================
    // Parse command line arguments
    // ========================================================================
    int port = mds::DEFAULT_PORT;
    int messages_per_second = 1000;  // Default: 1000 messages/sec

    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        messages_per_second = std::atoi(argv[2]);
    }

    mds::log_info("Configuration:");
    mds::log_info("  - TCP Port: {}", port);
    mds::log_info("  - Messages/sec: {}", messages_per_second);

    // Calculate sleep duration between messages
    auto sleep_duration = std::chrono::microseconds(1'000'000 / messages_per_second);

    // ========================================================================
    // Setup signal handlers for graceful shutdown
    // ========================================================================
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill command

    try {
        // ====================================================================
        // Step 1: Create shared memory with ring buffer
        // ====================================================================
        mds::log_info("Creating shared memory ring buffer...");

        using RingBuffer = mds::SPSCRingBuffer<mds::MarketData>;
        auto shm = mds::SharedMemory<RingBuffer>::create();

        // Initialize the ring buffer
        shm->init();

        mds::log_info("Ring buffer initialized (capacity: {} messages)", RingBuffer::capacity());

        // ====================================================================
        // Step 2: Create TCP server
        // ====================================================================
        mds::log_info("Starting TCP server on {}:{}...", mds::LOOPBACK_ADDR, port);

        mds::TCPServer server(port);

        mds::log_info("Waiting for TCP client to connect...");
        mds::log_info("(Start the tcp_consumer in another terminal)");

        // Wait for a client to connect (blocking)
        int client_fd = server.accept_client();

        // ====================================================================
        // Step 3: Create market data generator
        // ====================================================================
        // RELIANCE is a popular Indian stock (as shown in assignment)
        MarketDataGenerator generator("RELIANCE", 2850.0);

        mds::log_info("Starting to publish market data...");
        mds::log_info("Press Ctrl+C to stop");
        mds::log_info("-------------------------------------------");

        // ====================================================================
        // Step 4: Main publishing loop
        // ====================================================================
        uint64_t message_count = 0;
        uint64_t shm_push_failures = 0;
        uint64_t tcp_send_failures = 0;

        while (g_running) {
            // Generate market data
            mds::MarketData data = generator.generate();

            // ----------------------------------------------------------------
            // Publish to Shared Memory (for Process B)
            // ----------------------------------------------------------------
            if (!shm->push(data)) {
                // Buffer full - consumer not keeping up
                shm_push_failures++;
                // Don't log every failure - would flood the output
                if (shm_push_failures % 10000 == 1) {
                    mds::log_error("SHM ring buffer full! Consumer not keeping up.");
                }
            }

            // ----------------------------------------------------------------
            // Publish to TCP (for Process C)
            // ----------------------------------------------------------------
            std::string json_str = to_json(data);
            ssize_t sent = mds::TCPServer::send_data(client_fd, json_str.c_str(), json_str.size());

            if (sent < 0) {
                tcp_send_failures++;
                if (tcp_send_failures == 1) {
                    mds::log_error("TCP send failed: {} - client disconnected?", strerror(errno));
                    // Client disconnected - wait for new client
                    mds::TCPServer::close_client(client_fd);
                    mds::log_info("Waiting for new TCP client...");
                    client_fd = server.accept_client();
                    tcp_send_failures = 0;
                }
            }

            // ----------------------------------------------------------------
            // Log progress periodically
            // ----------------------------------------------------------------
            message_count++;
            if (message_count % 10000 == 0) {
                mds::log_info("Published {} messages (SHM failures: {}, TCP failures: {})",
                             message_count, shm_push_failures, tcp_send_failures);
            }

            // ----------------------------------------------------------------
            // Rate limiting - sleep to maintain target messages/second
            // ----------------------------------------------------------------
            // Note: For maximum performance, remove this sleep!
            // But it helps demonstrate the system without overwhelming.
            std::this_thread::sleep_for(sleep_duration);
        }

        // ====================================================================
        // Cleanup
        // ====================================================================
        mds::log_info("-------------------------------------------");
        mds::log_info("Shutting down...");
        mds::log_info("Total messages published: {}", message_count);
        mds::log_info("SHM push failures: {}", shm_push_failures);
        mds::log_info("TCP send failures: {}", tcp_send_failures);

        mds::TCPServer::close_client(client_fd);

        // SharedMemory destructor will unlink the shared memory

    } catch (const std::exception& e) {
        mds::log_error("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }

    mds::log_info("Publisher terminated cleanly");
    return EXIT_SUCCESS;
}
