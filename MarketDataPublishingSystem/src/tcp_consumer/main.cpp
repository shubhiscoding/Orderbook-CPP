#include <csignal>
#include <atomic>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "common/market_data.hpp"
#include "common/tcp_socket.hpp"
#include "common/cpu_affinity.hpp"
#include "common/logger.hpp"

using json = nlohmann::json;

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

bool parse_market_data(const std::string& json_str, mds::MarketData& data) {
    try {
        auto j = json::parse(json_str);

        std::string instrument = j["instrument"].get<std::string>();
        std::strncpy(data.instrument, instrument.c_str(), mds::INSTRUMENT_NAME_SIZE - 1);
        data.instrument[mds::INSTRUMENT_NAME_SIZE - 1] = '\0';

        data.bid = j["bid"].get<double>();
        data.ask = j["ask"].get<double>();
        data.timestamp_ns = j["timestamp_ns"].get<int64_t>();

        return true;
    } catch (const json::exception& e) {
        return false;
    }
}

int main(int argc, char* argv[]) {
    mds::log_info("===========================================");
    mds::log_info("  TCP Consumer (Process C)");
    mds::log_info("===========================================");

    const char* host = mds::LOOPBACK_ADDR;
    int port = mds::DEFAULT_PORT;
    int cpu_core = 2;

    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        cpu_core = std::atoi(argv[2]);
    }

    mds::print_cpu_info();
    mds::set_cpu_affinity(cpu_core);

    mds::log_info("Configuration:");
    mds::log_info("  - Server: {}:{}", host, port);
    mds::log_info("  - CPU Core: {}", cpu_core);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        mds::log_info("Connecting to publisher...");
        mds::log_info("(Make sure the publisher is running first!)");

        mds::TCPClient client(host, port);

        mds::log_info("Successfully connected to publisher");
        mds::log_info("-------------------------------------------");
        mds::log_info("Receiving market data...");
        mds::log_info("(Press Ctrl+C to stop)");
        mds::log_info("");

        uint64_t message_count = 0;
        uint64_t parse_errors = 0;
        std::string buffer;

        constexpr size_t RECV_BUFFER_SIZE = 4096;
        char recv_buffer[RECV_BUFFER_SIZE];

        while (g_running) {
            ssize_t bytes_received = client.receive(recv_buffer, RECV_BUFFER_SIZE - 1);

            if (bytes_received < 0) {
                mds::log_error("Receive error: {}", strerror(errno));
                break;
            }

            if (bytes_received == 0) {
                mds::log_info("Server closed connection");
                break;
            }

            recv_buffer[bytes_received] = '\0';
            buffer += recv_buffer;

            size_t newline_pos;
            while ((newline_pos = buffer.find('\n')) != std::string::npos) {
                std::string json_str = buffer.substr(0, newline_pos);
                buffer.erase(0, newline_pos + 1);

                if (json_str.empty()) {
                    continue;
                }

                mds::MarketData data;
                if (parse_market_data(json_str, data)) {
                    int64_t now = mds::get_timestamp_ns();
                    int64_t latency_ns = now - data.timestamp_ns;

                    mds::log_market_data(data.instrument, data.bid, data.ask, data.timestamp_ns);

                    message_count++;
                    if (message_count % 1000 == 0) {
                        double latency_us = latency_ns / 1000.0;
                        mds::log_info("  [Stats] Received {} messages, last latency: {:.2f} us ({} ns)",
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
