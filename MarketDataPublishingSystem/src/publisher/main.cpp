#include <csignal>
#include <chrono>
#include <thread>
#include <random>
#include <atomic>
#include <cstdlib>
#include <poll.h>

#include <nlohmann/json.hpp>

#include "common/market_data.hpp"
#include "common/ring_buffer.hpp"
#include "common/shared_memory.hpp"
#include "common/tcp_socket.hpp"
#include "common/cpu_affinity.hpp"
#include "common/logger.hpp"

using json = nlohmann::json;

std::atomic<bool> g_running{true};

void signal_handler(int signum) {
    mds::log_info("Received signal {} - shutting down...", signum);
    g_running = false;
}

class MarketDataGenerator {
public:
    MarketDataGenerator(const char* instrument,
                        double base_price,
                        double volatility = 0.001,
                        double spread = 0.0002)
        : instrument_(instrument)
        , current_price_(base_price)
        , volatility_(volatility)
        , spread_(spread)
        , rng_(std::random_device{}())
        , dist_(-1.0, 1.0) {
    }

    mds::MarketData generate() {
        double price_change = dist_(rng_) * volatility_ * current_price_;
        current_price_ += price_change;

        if (current_price_ < 1.0) {
            current_price_ = 1.0;
        }

        double half_spread = current_price_ * spread_ / 2.0;
        double bid = current_price_ - half_spread;
        double ask = current_price_ + half_spread;

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

std::string to_json(const mds::MarketData& data) {
    json j;
    j["instrument"] = data.instrument;
    j["bid"] = data.bid;
    j["ask"] = data.ask;
    j["timestamp_ns"] = data.timestamp_ns;

    return j.dump() + "\n";
}

int main(int argc, char* argv[]) {
    mds::log_info("===========================================");
    mds::log_info("  Market Data Publisher (Process A)");
    mds::log_info("===========================================");

    int port = mds::DEFAULT_PORT;
    int messages_per_second = 1000;
    int cpu_core = 0;

    if (argc >= 2) {
        port = std::atoi(argv[1]);
    }
    if (argc >= 3) {
        messages_per_second = std::atoi(argv[2]);
    }
    if (argc >= 4) {
        cpu_core = std::atoi(argv[3]);
    }

    mds::print_cpu_info();
    mds::set_cpu_affinity(cpu_core);

    mds::log_info("Configuration:");
    mds::log_info("  - TCP Port: {}", port);
    mds::log_info("  - Messages/sec: {}", messages_per_second);
    mds::log_info("  - CPU Core: {}", cpu_core);

    auto sleep_duration = std::chrono::microseconds(1'000'000 / messages_per_second);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        mds::log_info("Creating shared memory ring buffer...");

        using RingBuffer = mds::SPSCRingBuffer<mds::MarketData>;
        auto shm = mds::SharedMemory<RingBuffer>::create();

        shm->init();

        mds::log_info("Ring buffer initialized (capacity: {} messages)", RingBuffer::capacity());

        mds::log_info("Starting TCP server on {}:{}...", mds::LOOPBACK_ADDR, port);

        mds::TCPServer server(port);

        int client_fd = -1;

        mds::log_info("TCP server ready (client connection is optional)");

        MarketDataGenerator generator("RELIANCE", 2850.0);

        mds::log_info("Starting to publish market data...");
        mds::log_info("SHM Consumer can connect now!");
        mds::log_info("TCP Consumer can connect anytime to {}:{}", mds::LOOPBACK_ADDR, port);
        mds::log_info("Press Ctrl+C to stop");
        mds::log_info("-------------------------------------------");

        uint64_t message_count = 0;
        uint64_t shm_push_failures = 0;
        uint64_t tcp_send_failures = 0;

        while (g_running) {
            if (client_fd < 0) {
                struct pollfd pfd;
                pfd.fd = server.fd();
                pfd.events = POLLIN;

                int poll_result = poll(&pfd, 1, 0);
                if (poll_result > 0 && (pfd.revents & POLLIN)) {
                    try {
                        client_fd = server.accept_client();
                        mds::log_info("TCP client connected! Now sending to both SHM and TCP.");
                    } catch (const std::exception& e) {
                        mds::log_error("Failed to accept client: {}", e.what());
                    }
                }
            }

            mds::MarketData data = generator.generate();

            if (!shm->push(data)) {
                shm_push_failures++;
                if (shm_push_failures % 10000 == 1) {
                    mds::log_error("SHM ring buffer full! Consumer not keeping up.");
                }
            }

            if (client_fd >= 0) {
                std::string json_str = to_json(data);
                ssize_t sent = mds::TCPServer::send_data(client_fd, json_str.c_str(), json_str.size());

                if (sent < 0) {
                    tcp_send_failures++;
                    mds::log_error("TCP send failed - client disconnected");
                    mds::TCPServer::close_client(client_fd);
                    client_fd = -1;
                }
            }

            message_count++;
            if (message_count % 10000 == 0) {
                mds::log_info("Published {} messages (SHM failures: {}, TCP failures: {}, TCP connected: {})",
                             message_count, shm_push_failures, tcp_send_failures, client_fd >= 0 ? "yes" : "no");
            }

            std::this_thread::sleep_for(sleep_duration);
        }

        mds::log_info("-------------------------------------------");
        mds::log_info("Shutting down...");
        mds::log_info("Total messages published: {}", message_count);
        mds::log_info("SHM push failures: {}", shm_push_failures);
        mds::log_info("TCP send failures: {}", tcp_send_failures);

        if (client_fd >= 0) {
            mds::TCPServer::close_client(client_fd);
        }

    } catch (const std::exception& e) {
        mds::log_error("Fatal error: {}", e.what());
        return EXIT_FAILURE;
    }

    mds::log_info("Publisher terminated cleanly");
    return EXIT_SUCCESS;
}
