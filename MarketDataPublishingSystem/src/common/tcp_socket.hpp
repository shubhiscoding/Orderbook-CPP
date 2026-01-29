#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <stdexcept>

#include "logger.hpp"

namespace mds {

constexpr int DEFAULT_PORT = 9000;
constexpr const char* LOOPBACK_ADDR = "127.0.0.1";

class TCPServer {
public:
    explicit TCPServer(int port = DEFAULT_PORT, int backlog = 5) : port_(port) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ == -1) {
            throw std::runtime_error(
                fmt::format("socket() failed: {}", strerror(errno)));
        }

        log_info("Created TCP socket (fd={})", server_fd_);

        int opt = 1;
        if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("setsockopt(SO_REUSEADDR) failed: {}", strerror(errno)));
        }

        log_info("Set SO_REUSEADDR option");

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("bind() failed on port {}: {}", port, strerror(errno)));
        }

        log_info("Bound to {}:{}", LOOPBACK_ADDR, port);

        if (listen(server_fd_, backlog) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("listen() failed: {}", strerror(errno)));
        }

        log_info("Listening for connections (backlog={})", backlog);
    }

    ~TCPServer() {
        if (server_fd_ != -1) {
            close(server_fd_);
            log_info("Closed server socket");
        }
    }

    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;

    TCPServer(TCPServer&& other) noexcept
        : server_fd_(other.server_fd_), port_(other.port_) {
        other.server_fd_ = -1;
    }

    int accept_client() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &client_len);

        if (client_fd < 0) {
            throw std::runtime_error(
                fmt::format("accept() failed: {}", strerror(errno)));
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        log_info("Accepted client from {}:{} (fd={})",
                 client_ip, ntohs(client_addr.sin_port), client_fd);

        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            log_error("Warning: Failed to set TCP_NODELAY: {}", strerror(errno));
        } else {
            log_info("Enabled TCP_NODELAY on client socket");
        }

        return client_fd;
    }

    static ssize_t send_data(int client_fd, const void* data, size_t len) {
        return send(client_fd, data, len, MSG_NOSIGNAL);
    }

    static ssize_t send_string(int client_fd, const std::string& str) {
        return send_data(client_fd, str.c_str(), str.size());
    }

    static void close_client(int client_fd) {
        if (client_fd >= 0) {
            close(client_fd);
            log_info("Closed client connection (fd={})", client_fd);
        }
    }

    int fd() const { return server_fd_; }
    int port() const { return port_; }

private:
    int server_fd_ = -1;
    int port_ = 0;
};

class TCPClient {
public:
    TCPClient(const char* host = LOOPBACK_ADDR, int port = DEFAULT_PORT) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == -1) {
            throw std::runtime_error(
                fmt::format("socket() failed: {}", strerror(errno)));
        }

        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
            close(fd_);
            throw std::runtime_error(
                fmt::format("Invalid address: {}", host));
        }

        if (connect(fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
                    sizeof(server_addr)) < 0) {
            close(fd_);
            throw std::runtime_error(
                fmt::format("connect() failed: {} - Is the publisher running?",
                           strerror(errno)));
        }

        log_info("Connected to {}:{}", host, port);

        int flag = 1;
        if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            log_error("Warning: Failed to set TCP_NODELAY: {}", strerror(errno));
        }
    }

    ~TCPClient() {
        if (fd_ >= 0) {
            close(fd_);
            log_info("Closed TCP client connection");
        }
    }

    TCPClient(const TCPClient&) = delete;
    TCPClient& operator=(const TCPClient&) = delete;

    TCPClient(TCPClient&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    ssize_t receive(void* buffer, size_t max_len) {
        return recv(fd_, buffer, max_len, 0);
    }

    std::string receive_string(size_t max_len = 4096) {
        std::string buffer(max_len, '\0');
        ssize_t bytes = receive(buffer.data(), max_len);
        if (bytes > 0) {
            buffer.resize(bytes);
            return buffer;
        }
        return "";
    }

    int fd() const { return fd_; }

private:
    int fd_ = -1;
};

}  