#pragma once

/**
 * ring_buffer.hpp
 *
 * Lock-Free Single Producer Single Consumer (SPSC) Ring Buffer
 *
 * This is the CORE low-latency component of our market data system.
 *
 * Key Design Decisions:
 * =====================
 *
 * 1. LOCK-FREE: No mutexes! We use std::atomic with careful memory ordering.
 *    - Only works because we have exactly ONE producer and ONE consumer
 *    - Producer only modifies write_idx
 *    - Consumer only modifies read_idx
 *
 * 2. CACHE-LINE PADDING: Using alignas(64) to prevent false sharing
 *    - CPU caches work in 64-byte chunks called "cache lines"
 *    - If write_idx and read_idx share a cache line, modifying one
 *      invalidates the other's cache → huge performance hit!
 *    - By aligning to 64 bytes, each index gets its own cache line
 *
 * 3. POWER-OF-2 SIZE: Buffer size MUST be a power of 2
 *    - Allows us to use bitwise AND instead of modulo for wrapping
 *    - index % 1024 → slow (requires division)
 *    - index & (1024-1) → fast (single CPU instruction)
 *
 * 4. MEMORY ORDERING: Using acquire-release semantics (BONUS POINTS!)
 *    - memory_order_release: "publish" - all prior writes visible to acquirer
 *    - memory_order_acquire: "subscribe" - sees all writes before the release
 *    - Much faster than sequential consistency (the default)
 *
 * How it works:
 * =============
 *
 *   write_idx (producer)
 *       ↓
 * ┌───┬───┬───┬───┬───┬───┬───┬───┐
 * │ 5 │ 6 │   │   │   │ 2 │ 3 │ 4 │  ← data[i] = buffer[i & (SIZE-1)]
 * └───┴───┴───┴───┴───┴───┴───┴───┘
 *                       ↑
 *                   read_idx (consumer)
 *
 * - Producer writes to buffer[write_idx & MASK], then increments write_idx
 * - Consumer reads from buffer[read_idx & MASK], then increments read_idx
 * - Buffer is FULL when: write_idx - read_idx == BUFFER_SIZE
 * - Buffer is EMPTY when: write_idx == read_idx
 */

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>  // for std::hardware_destructive_interference_size

namespace mds {

// ============================================================================
// Configuration
// ============================================================================

// Buffer size MUST be a power of 2 for fast modulo operation
// 1024 entries = 1024 * 40 bytes = ~40KB of market data
constexpr size_t RING_BUFFER_SIZE = 1024;

// Mask for fast modulo: index & MASK == index % SIZE (when SIZE is power of 2)
constexpr size_t RING_BUFFER_MASK = RING_BUFFER_SIZE - 1;

// Cache line size - 64 bytes on most modern x86/ARM CPUs
// Used for padding to prevent false sharing
constexpr size_t CACHE_LINE_SIZE = 64;

// Compile-time check: ensure buffer size is power of 2
static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE - 1)) == 0,
              "RING_BUFFER_SIZE must be a power of 2");

// ============================================================================
// Ring Buffer Structure (for Shared Memory)
// ============================================================================

/**
 * SPSCRingBuffer - The actual ring buffer stored in shared memory
 *
 * Template parameter T is the message type (MarketData in our case)
 *
 * IMPORTANT: This struct is placed in shared memory via mmap()
 * - No virtual functions (vtable pointers don't work across processes)
 * - No std::string or pointers (addresses differ between processes)
 * - Fixed size only!
 */
template <typename T, size_t Size = RING_BUFFER_SIZE>
struct SPSCRingBuffer {
    // ========================================================================
    // Index Variables - Each on its own cache line!
    // ========================================================================

    // Producer's write position
    // alignas(64) ensures this starts at a 64-byte boundary
    // The padding after it ensures nothing else shares this cache line
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_idx;

    // Consumer's read position
    // On a SEPARATE cache line from write_idx
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_idx;

    // ========================================================================
    // Data Buffer - Also cache-line aligned for optimal access
    // ========================================================================
    alignas(CACHE_LINE_SIZE) T buffer[Size];

    // ========================================================================
    // Mask for index wrapping (stored for convenience)
    // ========================================================================
    static constexpr size_t MASK = Size - 1;

    // ========================================================================
    // Initialization (called by producer when creating shared memory)
    // ========================================================================

    /**
     * Initialize the ring buffer
     * MUST be called once by the producer before any operations
     */
    void init() {
        write_idx.store(0, std::memory_order_relaxed);
        read_idx.store(0, std::memory_order_relaxed);
        // Initialize buffer elements using default constructor
        // (safer than memset for non-trivial types)
        for (size_t i = 0; i < Size; ++i) {
            buffer[i] = T{};
        }
    }

    // ========================================================================
    // Producer Operations (called by Process A)
    // ========================================================================

    /**
     * Try to push an item into the buffer
     *
     * @param item The data to push
     * @return true if successful, false if buffer is full
     *
     * Memory Ordering Explanation:
     * - We load read_idx with acquire to see the consumer's latest position
     * - We store write_idx with release to publish our write to the consumer
     */
    bool push(const T& item) {
        // Load our write position (relaxed - only we modify it)
        const size_t current_write = write_idx.load(std::memory_order_relaxed);

        // Load consumer's read position (acquire - need to see their progress)
        const size_t current_read = read_idx.load(std::memory_order_acquire);

        // Check if buffer is full
        // Full condition: write has wrapped around and caught up to read
        if (current_write - current_read >= Size) {
            return false;  // Buffer full, cannot push
        }

        // Write the data to the buffer
        // Use bitwise AND for fast modulo (index & MASK)
        buffer[current_write & MASK] = item;

        // Publish the write by incrementing write_idx
        // Release ensures the data write above is visible before this
        write_idx.store(current_write + 1, std::memory_order_release);

        return true;
    }

    /**
     * Push with spin-wait (blocks until space available)
     * Use with caution - can waste CPU cycles!
     *
     * @param item The data to push
     */
    void push_blocking(const T& item) {
        while (!push(item)) {
            // Spin wait - could add pause instruction for efficiency
            // _mm_pause() on x86 reduces power and improves performance
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }

    // ========================================================================
    // Consumer Operations (called by Process B)
    // ========================================================================

    /**
     * Try to pop an item from the buffer
     *
     * @param item Output parameter - filled with data if successful
     * @return true if successful, false if buffer is empty
     *
     * Memory Ordering Explanation:
     * - We load write_idx with acquire to see the producer's latest write
     * - We store read_idx with release to publish our read to the producer
     */
    bool pop(T& item) {
        // Load our read position (relaxed - only we modify it)
        const size_t current_read = read_idx.load(std::memory_order_relaxed);

        // Load producer's write position (acquire - need to see their data)
        const size_t current_write = write_idx.load(std::memory_order_acquire);

        // Check if buffer is empty
        if (current_read >= current_write) {
            return false;  // Buffer empty, nothing to read
        }

        // Read the data from the buffer
        item = buffer[current_read & MASK];

        // Publish the read by incrementing read_idx
        // Release ensures the data read above completes before this
        read_idx.store(current_read + 1, std::memory_order_release);

        return true;
    }

    /**
     * Pop with spin-wait (blocks until data available)
     * Use with caution - can waste CPU cycles!
     *
     * @param item Output parameter - filled with data
     */
    void pop_blocking(T& item) {
        while (!pop(item)) {
            // Spin wait
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }

    // ========================================================================
    // Utility Functions
    // ========================================================================

    /**
     * Check if buffer is empty (approximate - may race)
     * Useful for debugging, not for synchronization
     */
    bool empty() const {
        return read_idx.load(std::memory_order_relaxed) >=
               write_idx.load(std::memory_order_relaxed);
    }

    /**
     * Check if buffer is full (approximate - may race)
     */
    bool full() const {
        return (write_idx.load(std::memory_order_relaxed) -
                read_idx.load(std::memory_order_relaxed)) >= Size;
    }

    /**
     * Get number of items in buffer (approximate - may race)
     */
    size_t size() const {
        const size_t w = write_idx.load(std::memory_order_relaxed);
        const size_t r = read_idx.load(std::memory_order_relaxed);
        return w >= r ? w - r : 0;
    }

    /**
     * Get buffer capacity
     */
    static constexpr size_t capacity() { return Size; }
};

// ============================================================================
// Type alias for our market data ring buffer
// ============================================================================
// Note: Include market_data.hpp before using MarketDataRingBuffer
// We don't include it here to avoid circular dependencies
// Usage:
//   #include "market_data.hpp"
//   #include "ring_buffer.hpp"
//   mds::SPSCRingBuffer<mds::MarketData> ring_buffer;

}  // namespace mds
