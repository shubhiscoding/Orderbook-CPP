#pragma once

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include "logger.hpp"

namespace mds {

constexpr const char* SHM_NAME = "/market_data_ring_buffer";

template <typename T>
class SharedMemory {
public:
    static SharedMemory create(const char* name = SHM_NAME) {
        return SharedMemory(name, true);
    }

    static SharedMemory open(const char* name = SHM_NAME) {
        return SharedMemory(name, false);
    }

    ~SharedMemory() {
        cleanup();
    }

    SharedMemory(SharedMemory&& other) noexcept
        : ptr_(other.ptr_)
        , fd_(other.fd_)
        , size_(other.size_)
        , is_owner_(other.is_owner_)
        , name_(std::move(other.name_)) {
        other.ptr_ = nullptr;
        other.fd_ = -1;
        other.is_owner_ = false;
    }

    SharedMemory& operator=(SharedMemory&& other) noexcept {
        if (this != &other) {
            cleanup();
            ptr_ = other.ptr_;
            fd_ = other.fd_;
            size_ = other.size_;
            is_owner_ = other.is_owner_;
            name_ = std::move(other.name_);
            other.ptr_ = nullptr;
            other.fd_ = -1;
            other.is_owner_ = false;
        }
        return *this;
    }

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    T* get() { return ptr_; }
    const T* get() const { return ptr_; }

    T* operator->() { return ptr_; }
    const T* operator->() const { return ptr_; }
    T& operator*() { return *ptr_; }
    const T& operator*() const { return *ptr_; }

    void unlink() {
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
            log_info("Shared memory '{}' unlinked", name_);
        }
    }

private:
    T* ptr_ = nullptr;
    int fd_ = -1;
    size_t size_ = sizeof(T);
    bool is_owner_ = false;
    std::string name_;

    SharedMemory(const char* name, bool create_new) : name_(name) {
        is_owner_ = create_new;

        if (create_new) {
            shm_unlink(name);

            fd_ = shm_open(name, O_CREAT | O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error(
                    fmt::format("shm_open(create) failed: {}", strerror(errno)));
            }

            if (ftruncate(fd_, size_) == -1) {
                close(fd_);
                shm_unlink(name);
                throw std::runtime_error(
                    fmt::format("ftruncate failed: {}", strerror(errno)));
            }

            log_info("Created shared memory '{}' ({} bytes)", name, size_);

        } else {
            fd_ = shm_open(name, O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error(
                    fmt::format("shm_open(open) failed: {} - Is the publisher running?",
                               strerror(errno)));
            }

            log_info("Opened existing shared memory '{}'", name);
        }

        void* mapped = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);

        if (mapped == MAP_FAILED) {
            close(fd_);
            if (is_owner_) {
                shm_unlink(name);
            }
            throw std::runtime_error(
                fmt::format("mmap failed: {}", strerror(errno)));
        }

        ptr_ = static_cast<T*>(mapped);

        if (is_owner_) {
            new (ptr_) T();
            log_info("Initialized shared memory object");
        }

        log_info("Mapped {} bytes at address {}", size_, static_cast<void*>(ptr_));
    }

    void cleanup() {
        if (ptr_ != nullptr) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
            log_info("Unmapped shared memory");
        }

        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }

        if (is_owner_ && !name_.empty()) {
            shm_unlink(name_.c_str());
            log_info("Unlinked shared memory '{}'", name_);
        }
    }
};

}  // namespace mds
