#pragma once

/**
 * shared_memory.hpp
 *
 * Utilities for creating and accessing shared memory regions.
 *
 * How Shared Memory Works:
 * ========================
 *
 * Normally, each process has its own virtual address space:
 *
 *   Process A          Process B
 *   ┌─────────┐       ┌─────────┐
 *   │ Stack   │       │ Stack   │
 *   │ Heap    │       │ Heap    │
 *   │ Data    │       │ Data    │
 *   │ Code    │       │ Code    │
 *   └─────────┘       └─────────┘
 *       ↓                  ↓
 *   Separate physical memory
 *
 * With shared memory, both processes map the SAME physical memory:
 *
 *   Process A          Process B
 *   ┌─────────┐       ┌─────────┐
 *   │ Stack   │       │ Stack   │
 *   │ Heap    │       │ Heap    │
 *   │ SHM ────┼───────┼── SHM   │  ← Same physical memory!
 *   │ Data    │       │ Data    │
 *   │ Code    │       │ Code    │
 *   └─────────┘       └─────────┘
 *
 * Key Functions:
 * ==============
 *
 * shm_open(name, flags, mode)
 *   - Creates or opens a named shared memory object
 *   - Returns a file descriptor
 *   - Name must start with '/' (e.g., "/market_data")
 *
 * ftruncate(fd, size)
 *   - Sets the size of the shared memory region
 *   - MUST be called when creating new shared memory
 *
 * mmap(addr, length, prot, flags, fd, offset)
 *   - Maps the shared memory into process address space
 *   - Returns a pointer we can use like regular memory
 *   - prot: PROT_READ | PROT_WRITE (read/write access)
 *   - flags: MAP_SHARED (changes visible to other processes)
 *
 * munmap(addr, length)
 *   - Unmaps the shared memory from process
 *
 * shm_unlink(name)
 *   - Removes the shared memory object
 *   - Should be called by producer when done
 */

#include <sys/mman.h>   // mmap, munmap, shm_open, shm_unlink
#include <sys/stat.h>   // mode constants
#include <fcntl.h>      // O_* constants
#include <unistd.h>     // ftruncate, close
#include <cerrno>       // errno
#include <cstring>      // strerror
#include <stdexcept>    // std::runtime_error
#include <string>

#include "logger.hpp"

namespace mds {

// Default name for our market data shared memory region
constexpr const char* SHM_NAME = "/market_data_ring_buffer";

/**
 * SharedMemory - RAII wrapper for shared memory management
 *
 * RAII = Resource Acquisition Is Initialization
 * - Constructor acquires the resource (opens/creates shared memory)
 * - Destructor releases the resource (unmaps/closes)
 * - Prevents memory leaks even if exceptions occur
 *
 * Template parameter T: the type to store in shared memory (our RingBuffer)
 */
template <typename T>
class SharedMemory {
public:
    /**
     * Create new shared memory (for Producer/Process A)
     *
     * @param name Name of shared memory object (must start with '/')
     * @return SharedMemory instance owning the created region
     */
    static SharedMemory create(const char* name = SHM_NAME) {
        return SharedMemory(name, true);
    }

    /**
     * Open existing shared memory (for Consumer/Process B)
     *
     * @param name Name of shared memory object (must start with '/')
     * @return SharedMemory instance attached to existing region
     */
    static SharedMemory open(const char* name = SHM_NAME) {
        return SharedMemory(name, false);
    }

    // Destructor - cleanup resources
    ~SharedMemory() {
        cleanup();
    }

    // Move constructor
    SharedMemory(SharedMemory&& other) noexcept
        : ptr_(other.ptr_)
        , fd_(other.fd_)
        , size_(other.size_)
        , is_owner_(other.is_owner_)
        , name_(std::move(other.name_)) {
        // Prevent other from cleaning up
        other.ptr_ = nullptr;
        other.fd_ = -1;
        other.is_owner_ = false;
    }

    // Move assignment
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

    // Disable copy (shared memory handle should be unique per process)
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    /**
     * Get pointer to the shared data
     * This is what we use to access the ring buffer
     */
    T* get() { return ptr_; }
    const T* get() const { return ptr_; }

    // Convenience operators
    T* operator->() { return ptr_; }
    const T* operator->() const { return ptr_; }
    T& operator*() { return *ptr_; }
    const T& operator*() const { return *ptr_; }

    /**
     * Explicitly unlink (remove) the shared memory
     * Normally called by producer when shutting down
     */
    void unlink() {
        if (!name_.empty()) {
            shm_unlink(name_.c_str());
            log_info("Shared memory '{}' unlinked", name_);
        }
    }

private:
    T* ptr_ = nullptr;          // Pointer to mapped memory
    int fd_ = -1;               // File descriptor
    size_t size_ = sizeof(T);   // Size of mapped region
    bool is_owner_ = false;     // True if we created (and should unlink)
    std::string name_;          // Name of shared memory object

    /**
     * Private constructor - use create() or open() static methods
     *
     * @param name Shared memory name
     * @param create_new True to create, false to open existing
     */
    SharedMemory(const char* name, bool create_new) : name_(name) {
        is_owner_ = create_new;

        if (create_new) {
            // ================================================================
            // CREATE NEW SHARED MEMORY (Producer)
            // ================================================================

            // Remove any existing shared memory with same name
            shm_unlink(name);

            // Create new shared memory object
            // O_CREAT: Create if doesn't exist
            // O_EXCL: Fail if already exists (we just unlinked, so shouldn't)
            // O_RDWR: Open for reading and writing
            // 0666: Permissions (read/write for all)
            fd_ = shm_open(name, O_CREAT | O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error(
                    fmt::format("shm_open(create) failed: {}", strerror(errno)));
            }

            // Set the size of shared memory region
            // IMPORTANT: Must do this before mmap!
            if (ftruncate(fd_, size_) == -1) {
                close(fd_);
                shm_unlink(name);
                throw std::runtime_error(
                    fmt::format("ftruncate failed: {}", strerror(errno)));
            }

            log_info("Created shared memory '{}' ({} bytes)", name, size_);

        } else {
            // ================================================================
            // OPEN EXISTING SHARED MEMORY (Consumer)
            // ================================================================

            // Open existing shared memory object
            // O_RDWR: Open for reading and writing
            fd_ = shm_open(name, O_RDWR, 0666);
            if (fd_ == -1) {
                throw std::runtime_error(
                    fmt::format("shm_open(open) failed: {} - Is the publisher running?",
                               strerror(errno)));
            }

            log_info("Opened existing shared memory '{}'", name);
        }

        // ====================================================================
        // MAP THE SHARED MEMORY INTO OUR ADDRESS SPACE
        // ====================================================================

        // mmap parameters:
        // - nullptr: Let OS choose the address
        // - size_: How much to map
        // - PROT_READ | PROT_WRITE: We need both read and write
        // - MAP_SHARED: Changes are visible to other processes (CRUCIAL!)
        // - fd_: File descriptor from shm_open
        // - 0: Offset (start from beginning)
        void* mapped = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);

        if (mapped == MAP_FAILED) {
            close(fd_);
            if (is_owner_) {
                shm_unlink(name);
            }
            throw std::runtime_error(
                fmt::format("mmap failed: {}", strerror(errno)));
        }

        // Cast to our type
        ptr_ = static_cast<T*>(mapped);

        // Initialize if we're the creator
        if (is_owner_) {
            // Use placement new to construct object in shared memory
            // This calls T's default constructor (or init() for our ring buffer)
            new (ptr_) T();
            log_info("Initialized shared memory object");
        }

        log_info("Mapped {} bytes at address {}", size_, static_cast<void*>(ptr_));
    }

    /**
     * Cleanup resources
     */
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

        // Only the creator should unlink
        if (is_owner_ && !name_.empty()) {
            shm_unlink(name_.c_str());
            log_info("Unlinked shared memory '{}'", name_);
        }
    }
};

}  // namespace mds
