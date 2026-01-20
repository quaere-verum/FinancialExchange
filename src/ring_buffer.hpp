#pragma once
#include <atomic>
#include <memory>
#include <cstring>
#include <algorithm>

template<size_t Capacity>
class RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0);

    public:
        RingBuffer()
            : buffer_(std::make_unique<uint8_t[]>(Capacity)) 
            {}

        bool try_push(const void* data, size_t len) noexcept {
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);

            const size_t used = (head - tail) & mask_;
            if (Capacity - used < len) {
                return false;
            }

            copy_to_buffer(head, data, len);
            head_.store((head + len) & mask_, std::memory_order_release);
            return true;
        }

        std::pair<const uint8_t*, size_t> peek() const noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);

            if (tail == head) return {nullptr, 0};

            const size_t idx = tail & mask_;
            const size_t available = (head - tail) & mask_;
            const size_t to_end = Capacity - idx;

            return { buffer_.get() + idx, std::min(available, to_end) };
        }

        void advance_read_index(size_t n) noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            tail_.store((tail + n) & mask_, std::memory_order_release);
        }

    private:
        void copy_to_buffer(size_t pos, const void* src, size_t len) noexcept {
            const size_t idx = pos & mask_;
            const size_t to_end = Capacity - idx;

            if (to_end < len) {
                std::memcpy(buffer_.get() + idx, src, to_end);
                std::memcpy(buffer_.get(), (const uint8_t*)src + to_end, len - to_end);
            } else {
                std::memcpy(buffer_.get() + idx, src, len);
            }
        }

        static constexpr size_t mask_ = Capacity - 1;

        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};
        alignas(64) std::unique_ptr<uint8_t[]> buffer_;
};
