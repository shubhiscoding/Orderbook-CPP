/**
 * Process C - TCP Consumer
 *
 * This process receives market data over TCP (the network path).
 *
 * TCP vs SHM Comparison:
 * ======================
 *
 * This TCP consumer will have HIGHER LATENCY than the SHM consumer because:
 *
 * 1. SYSTEM CALLS: send() and recv() require kernel transitions
 *    - User space → Kernel space → User space
 *    - Each transition costs ~100-1000 nanoseconds
 *
 * 2. DATA COPYING: Data is copied multiple times
 *    - App → Kernel send buffer → Network stack → Kernel recv buffer → App
 *
 * 3. PROTOCOL OVERHEAD: TCP adds headers, checksums, ACKs
 *    - Even on loopback, the full TCP stack runs
 *
 * 4. JSON PARSING: We need to deserialize JSON strings
 *    - SHM uses binary structs (no parsing needed)
 *
 * Expected latency comparison:
 * - SHM: ~100-500 nanoseconds
 * - TCP: ~10-50 microseconds (20-100x slower!)
 *
 * Why use TCP then?
 * - Works across machines (not just same host)
 * - Standard protocol (any language can connect)
 * - Useful for monitoring, debugging, external systems
 *
 * Message Format:
 * ===============
 * Publisher sends newline-delimited JSON:
 * {"instrument":"RELIANCE","bid":2850.25,"ask":2850.75,"timestamp_ns":123456789}\n
 * {"instrument":"RELIANCE","bid":2850.30,"ask":2850.80,"timestamp_ns":123456790}\n
 *
 * We buffer incoming data and parse complete JSON objects.
 */

#include <csignal>    // signal handling
#include <atomic>     // atomic flag
#include <cstdlib>    // exit codes
#include <string>     // string handling

#include <nlohmann/json.hpp>  // JSON parsing

#include "common/market_data.hpp"
#include "common/tcp_socket.hpp"
#include "common/logger.hpp"

using json = nlohmann::json;

// ============================================================================
// Global shutdown flag
// ============================================================================
std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

// ============================================================================
// JSON Parser Helper
// ============================================================================

/**
 * Parse a JSON string into MarketData
 *
 * Input: {"instrument":"RELIANCE","bid":2850.25,"ask":2850.75,"timestamp_ns":123}
 * Output: MarketData struct with fields populated
 */
bool parse_market_data(const std::string& json_str, mds::MarketData& data) {
    try {
        auto j = json::parse(json_str);

        // Extract fields
        std::string instrument = j["instrument"].get<std::string>();
        std::strncpy(data.instrument, instrument.c_str(), mds::INSTRUMENT_NAME_SIZE - 1);
        data.instrument[mds::INSTRUMENT_NAME_SIZE - 1] = '\0';

        data.bid = j["bid"].get<double>();
        data.ask = j["ask"].get<double>();
        data.timestamp_ns = j["timestamp_ns"].get<int64_t>();

        return true;
    } catch (const json::exception& e) {
        // JSON parsing failed - malformed data
        return false;
    }
}

// ============================================================================
// Main Function
// ============================================================================

int main(int argc, char* argv[]) {
    mds::log_info("===========================================");
    mds::log_info("  TCP Consumer (Process C)");
    mds::log_info("===========================================");

    // Parse command line arguments
    const char* host = mds::LOOPBACK_ADDR;
    int port = mds::DEFAULT_PORT;

    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }

    mds::log_info("Configuration:");
    mds::log_info("  - Server: {}:{}", host, port);

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        // ====================================================================
        // Step 1: Connect to publisher's TCP server
        // ====================================================================
        mds::log_info("Connecting to publisher...");
        mds::log_info("(Make sure the publisher is running first!)");

        mds::TCPClient client(host, port);

        mds::log_info("Successfully connected to publisher");
        mds::log_info("-------------------------------------------");
        mds::log_info("Receiving market data...");
        mds::log_info("(Press Ctrl+C to stop)");
        mds::log_info("");

        // ====================================================================
        // Step 2: Main receive loop
        // ====================================================================
        uint64_t message_count = 0;
        uint64_t parse_errors = 0;
        std::string buffer;  // Buffer for incomplete messages

        // Receive buffer
        constexpr size_t RECV_BUFFER_SIZE = 4096;
        char recv_buffer[RECV_BUFFER_SIZE];

        while (g_running) {
            // Receive data from server
            ssize_t bytes_received = client.receive(recv_buffer, RECV_BUFFER_SIZE - 1);

            if (bytes_received < 0) {
                mds::log_error("Receive error: {}", strerror(errno));
                break;
            }

            if (bytes_received == 0) {
                // Server closed connection
                mds::log_info("Server closed connection");
                break;
            }

            // Null-terminate and append to buffer
            recv_buffer[bytes_received] = '\0';
            buffer += recv_buffer;

            // ----------------------------------------------------------------
            // Process complete messages (newline-delimited JSON)
            // ----------------------------------------------------------------
            // Each JSON message ends with '\n'
            // Buffer might contain multiple messages or partial messages

            size_t newline_pos;
            while ((newline_pos = buffer.find('\n')) != std::string::npos) {
                // Extract one complete message
                std::string json_str = buffer.substr(0, newline_pos);
                buffer.erase(0, newline_pos + 1);  // Remove from buffer

                // Skip empty lines
                if (json_str.empty()) {
                    continue;
                }

                // Parse JSON into MarketData
                mds::MarketData data;
                if (parse_market_data(json_str, data)) {
                    // Calculate latency: current time - message timestamp
                    int64_t now = mds::get_timestamp_ns();
                    int64_t latency_ns = now - data.timestamp_ns;

                    // Log in the format specified by assignment:
                    // [12:34:56.123456789] RELIANCE BID=2850.25 ASK=2850.75
                    mds::log_market_data(data.instrument, data.bid, data.ask, data.timestamp_ns);

                    // Log latency periodically
                    message_count++;
                    if (message_count % 1000 == 0) {
                        // Convert latency to microseconds for readability
                        double latency_us = latency_ns / 1000.0;
                        mds::log_info("  [Stats] Received {} messages, last latency: {:.2f} µs ({} ns)",
                                     message_count, latency_us, latency_ns);
                    }
                } else {
                    parse_errors++;
                    if (parse_errors <= 5) {
                        mds::log_error("Failed to parse JSON: {}", json_str);
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
        mds::log_info("Parse errors: {}", parse_errors);

    } catch (const std::exception& e) {
        mds::log_error("Fatal error: {}", e.what());
        mds::log_error("Is the publisher (Process A) running?");
        return EXIT_FAILURE;
    }

    mds::log_info("TCP Consumer terminated cleanly");
    return EXIT_SUCCESS;
}
