#pragma once

/**
 * tcp_socket.hpp
 *
 * POSIX TCP Socket utilities for our market data system.
 *
 * POSIX Sockets Explained:
 * ========================
 *
 * A socket is a file descriptor that represents a network endpoint.
 * In Unix, "everything is a file" - sockets are no exception!
 *
 * Key Concepts:
 * -------------
 *
 * 1. ADDRESS FAMILY (AF_INET)
 *    - AF_INET = IPv4 addresses (what we use)
 *    - AF_INET6 = IPv6 addresses
 *
 * 2. SOCKET TYPE (SOCK_STREAM)
 *    - SOCK_STREAM = TCP (reliable, ordered, connection-based)
 *    - SOCK_DGRAM = UDP (unreliable, unordered, connectionless)
 *
 * 3. PROTOCOL (0)
 *    - 0 = Let the OS choose the appropriate protocol
 *    - For SOCK_STREAM, this means TCP
 *
 * 4. FILE DESCRIPTOR
 *    - socket() returns an int (fd) that represents the socket
 *    - Use this fd with send(), recv(), close(), etc.
 *    - fd is just a number that indexes into kernel's file table
 *
 * TCP vs UDP for Market Data:
 * ---------------------------
 * Real exchanges often use UDP multicast for market data because:
 * - Lower latency (no connection overhead)
 * - One-to-many broadcast
 *
 * But for this assignment, we use TCP because:
 * - Simpler to implement
 * - Guaranteed delivery
 * - Assignment specifically asks for TCP
 *
 * Nagle's Algorithm (TCP_NODELAY):
 * --------------------------------
 * By default, TCP buffers small packets and sends them together.
 * This is efficient for bulk transfers but BAD for low-latency!
 *
 * With Nagle enabled:
 *   App sends: [A] [B] [C]  (3 small writes)
 *   TCP sends: [ABC]        (1 combined packet after delay)
 *
 * With TCP_NODELAY:
 *   App sends: [A] [B] [C]  (3 small writes)
 *   TCP sends: [A] [B] [C]  (3 packets immediately)
 *
 * For market data, we ALWAYS want TCP_NODELAY!
 */

#include <sys/socket.h>  // socket, bind, listen, accept, send, recv
#include <netinet/in.h>  // sockaddr_in, INADDR_LOOPBACK
#include <netinet/tcp.h> // TCP_NODELAY
#include <arpa/inet.h>   // inet_ntoa, htons
#include <unistd.h>      // close
#include <fcntl.h>       // fcntl, F_SETFL, O_NONBLOCK
#include <cerrno>        // errno
#include <cstring>       // strerror, memset
#include <string>
#include <stdexcept>

#include "logger.hpp"

namespace mds {

// Default port for our market data server
constexpr int DEFAULT_PORT = 9000;

// Default loopback address
constexpr const char* LOOPBACK_ADDR = "127.0.0.1";

/**
 * TCPServer - RAII wrapper for a TCP server socket
 *
 * Usage:
 *   TCPServer server(9000);           // Binds and listens
 *   int client_fd = server.accept();  // Wait for client
 *   server.send(client_fd, data, len); // Send data
 */
class TCPServer {
public:
    /**
     * Create and start a TCP server
     *
     * @param port Port to listen on
     * @param backlog Maximum pending connections queue size
     */
    explicit TCPServer(int port = DEFAULT_PORT, int backlog = 5) : port_(port) {
        // ====================================================================
        // Step 1: Create socket
        // ====================================================================
        // socket(domain, type, protocol)
        // - AF_INET: IPv4
        // - SOCK_STREAM: TCP (reliable, ordered byte stream)
        // - 0: Let OS pick protocol (TCP for SOCK_STREAM)
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ == -1) {
            throw std::runtime_error(
                fmt::format("socket() failed: {}", strerror(errno)));
        }

        log_info("Created TCP socket (fd={})", server_fd_);

        // ====================================================================
        // Step 2: Set socket options
        // ====================================================================

        // SO_REUSEADDR: Allow reusing the address immediately after close
        // Without this, you get "Address already in use" errors on restart
        int opt = 1;
        if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("setsockopt(SO_REUSEADDR) failed: {}", strerror(errno)));
        }

        log_info("Set SO_REUSEADDR option");

        // ====================================================================
        // Step 3: Bind to address
        // ====================================================================
        // sockaddr_in structure:
        // - sin_family: Address family (AF_INET for IPv4)
        // - sin_port: Port number (network byte order - use htons!)
        // - sin_addr: IP address (INADDR_LOOPBACK = 127.0.0.1)

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);  // Host TO Network Short
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("bind() failed on port {}: {}", port, strerror(errno)));
        }

        log_info("Bound to {}:{}", LOOPBACK_ADDR, port);

        // ====================================================================
        // Step 4: Start listening
        // ====================================================================
        // listen(fd, backlog)
        // - backlog: Maximum number of pending connections in queue
        // - Once queue is full, new connections are refused

        if (listen(server_fd_, backlog) < 0) {
            close(server_fd_);
            throw std::runtime_error(
                fmt::format("listen() failed: {}", strerror(errno)));
        }

        log_info("Listening for connections (backlog={})", backlog);
    }

    // Destructor - close the server socket
    ~TCPServer() {
        if (server_fd_ != -1) {
            close(server_fd_);
            log_info("Closed server socket");
        }
    }

    // Disable copy
    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;

    // Move constructor
    TCPServer(TCPServer&& other) noexcept
        : server_fd_(other.server_fd_), port_(other.port_) {
        other.server_fd_ = -1;
    }

    /**
     * Accept a client connection (blocking)
     *
     * @return File descriptor for the connected client
     *
     * This blocks until a client connects!
     * The returned fd is used for send/recv with that specific client.
     */
    int accept_client() {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // accept() blocks until a client connects
        // Returns a NEW socket fd specifically for this client
        int client_fd = accept(server_fd_,
                               reinterpret_cast<struct sockaddr*>(&client_addr),
                               &client_len);

        if (client_fd < 0) {
            throw std::runtime_error(
                fmt::format("accept() failed: {}", strerror(errno)));
        }

        // Log client info
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        log_info("Accepted client from {}:{} (fd={})",
                 client_ip, ntohs(client_addr.sin_port), client_fd);

        // ====================================================================
        // Configure client socket for low latency (BONUS POINTS!)
        // ====================================================================

        // TCP_NODELAY: Disable Nagle's algorithm
        // This sends data immediately without waiting to combine small packets
        int flag = 1;
        if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            log_error("Warning: Failed to set TCP_NODELAY: {}", strerror(errno));
        } else {
            log_info("Enabled TCP_NODELAY on client socket");
        }

        return client_fd;
    }

    /**
     * Send data to a connected client
     *
     * @param client_fd Client socket file descriptor
     * @param data Pointer to data to send
     * @param len Length of data
     * @return Number of bytes sent, or -1 on error
     */
    static ssize_t send_data(int client_fd, const void* data, size_t len) {
        // send(fd, buffer, length, flags)
        // - MSG_NOSIGNAL: Don't generate SIGPIPE if client disconnects
        //   (otherwise our process would crash!)
        return send(client_fd, data, len, MSG_NOSIGNAL);
    }

    /**
     * Send a string to a connected client
     */
    static ssize_t send_string(int client_fd, const std::string& str) {
        return send_data(client_fd, str.c_str(), str.size());
    }

    /**
     * Close a client connection
     */
    static void close_client(int client_fd) {
        if (client_fd >= 0) {
            close(client_fd);
            log_info("Closed client connection (fd={})", client_fd);
        }
    }

    /**
     * Get the server's file descriptor
     */
    int fd() const { return server_fd_; }

    /**
     * Get the port we're listening on
     */
    int port() const { return port_; }

private:
    int server_fd_ = -1;  // Server socket file descriptor
    int port_ = 0;        // Port number
};


/**
 * TCPClient - RAII wrapper for a TCP client socket
 *
 * Usage:
 *   TCPClient client("127.0.0.1", 9000);  // Connect to server
 *   client.receive(buffer, size);          // Receive data
 */
class TCPClient {
public:
    /**
     * Create and connect a TCP client
     *
     * @param host Host to connect to
     * @param port Port to connect to
     */
    TCPClient(const char* host = LOOPBACK_ADDR, int port = DEFAULT_PORT) {
        // Create socket
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == -1) {
            throw std::runtime_error(
                fmt::format("socket() failed: {}", strerror(errno)));
        }

        // Setup server address
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        // Convert IP address from string to binary
        if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
            close(fd_);
            throw std::runtime_error(
                fmt::format("Invalid address: {}", host));
        }

        // Connect to server
        if (connect(fd_, reinterpret_cast<struct sockaddr*>(&server_addr),
                    sizeof(server_addr)) < 0) {
            close(fd_);
            throw std::runtime_error(
                fmt::format("connect() failed: {} - Is the publisher running?",
                           strerror(errno)));
        }

        log_info("Connected to {}:{}", host, port);

        // Enable TCP_NODELAY for low latency
        int flag = 1;
        if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            log_error("Warning: Failed to set TCP_NODELAY: {}", strerror(errno));
        }
    }

    // Destructor
    ~TCPClient() {
        if (fd_ >= 0) {
            close(fd_);
            log_info("Closed TCP client connection");
        }
    }

    // Disable copy
    TCPClient(const TCPClient&) = delete;
    TCPClient& operator=(const TCPClient&) = delete;

    // Move constructor
    TCPClient(TCPClient&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    /**
     * Receive data from server
     *
     * @param buffer Buffer to store received data
     * @param max_len Maximum bytes to receive
     * @return Number of bytes received, 0 if connection closed, -1 on error
     */
    ssize_t receive(void* buffer, size_t max_len) {
        return recv(fd_, buffer, max_len, 0);
    }

    /**
     * Receive data into a string (up to max_len bytes)
     */
    std::string receive_string(size_t max_len = 4096) {
        std::string buffer(max_len, '\0');
        ssize_t bytes = receive(buffer.data(), max_len);
        if (bytes > 0) {
            buffer.resize(bytes);
            return buffer;
        }
        return "";
    }

    /**
     * Get the socket file descriptor
     */
    int fd() const { return fd_; }

private:
    int fd_ = -1;  // Client socket file descriptor
};

}  // namespace mds
