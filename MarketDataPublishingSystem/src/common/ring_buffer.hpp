#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>

namespace mds {

constexpr size_t RING_BUFFER_SIZE = 1024;
constexpr size_t RING_BUFFER_MASK = RING_BUFFER_SIZE - 1;
constexpr size_t CACHE_LINE_SIZE = 64;

static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE - 1)) == 0,
              "RING_BUFFER_SIZE must be a power of 2");

template <typename T, size_t Size = RING_BUFFER_SIZE>
struct SPSCRingBuffer {
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_idx;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_idx;
    alignas(CACHE_LINE_SIZE) T buffer[Size];
    static constexpr size_t MASK = Size - 1;

    void init() {
        write_idx.store(0, std::memory_order_relaxed);
        read_idx.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < Size; ++i) {
            buffer[i] = T{};
        }
    }

    bool push(const T& item) {
        const size_t current_write = write_idx.load(std::memory_order_relaxed);
        const size_t current_read = read_idx.load(std::memory_order_acquire);

        if (current_write - current_read >= Size) {
            return false;
        }

        buffer[current_write & MASK] = item;
        write_idx.store(current_write + 1, std::memory_order_release);

        return true;
    }

    void push_blocking(const T& item) {
        while (!push(item)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }

    bool pop(T& item) {
        const size_t current_read = read_idx.load(std::memory_order_relaxed);
        const size_t current_write = write_idx.load(std::memory_order_acquire);

        if (current_read >= current_write) {
            return false;
        }

        item = buffer[current_read & MASK];
        read_idx.store(current_read + 1, std::memory_order_release);

        return true;
    }

    void pop_blocking(T& item) {
        while (!pop(item)) {
#if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
#endif
        }
    }

    bool empty() const {
        return read_idx.load(std::memory_order_relaxed) >=
               write_idx.load(std::memory_order_relaxed);
    }

    bool full() const {
        return (write_idx.load(std::memory_order_relaxed) -
                read_idx.load(std::memory_order_relaxed)) >= Size;
    }

    size_t size() const {
        const size_t w = write_idx.load(std::memory_order_relaxed);
        const size_t r = read_idx.load(std::memory_order_relaxed);
        return w >= r ? w - r : 0;
    }

    static constexpr size_t capacity() { return Size; }
};

}  // namespace mds
