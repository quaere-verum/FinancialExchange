#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

template <typename T, size_t CapacityPow2>
class SPSCQueue {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power of two");
    static_assert(CapacityPow2 >= 2, "Capacity must be >= 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for this SPSC queue.");

    public:
        SPSCQueue() noexcept = default;

        SPSCQueue(const SPSCQueue&) = delete;
        SPSCQueue& operator=(const SPSCQueue&) = delete;

        inline bool try_push(const T& item) noexcept {
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);

            if ((head - tail) >= CapacityPow2) {
                return false;
            }

            buffer_[head & mask_] = item;
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        inline bool try_push(T&& item) noexcept {
            return try_push(static_cast<const T&>(item));
        }

        inline bool try_pop(T& out) noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);

            if (tail == head) {
                return false;
            }

            out = buffer_[tail & mask_];
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        inline const T* peek() const noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);

            if (tail == head) return nullptr;
            return &buffer_[tail & mask_];
        }

        inline bool consume_one() noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);
            if (tail == head) return false;
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        inline size_t size_approx() const noexcept {
            const size_t head = head_.load(std::memory_order_acquire);
            const size_t tail = tail_.load(std::memory_order_acquire);
            return head - tail;
        }

        inline constexpr size_t capacity() const noexcept { return CapacityPow2; }

    private:
        static constexpr size_t mask_ = CapacityPow2 - 1;

        alignas(64) std::atomic<size_t> head_{0}; // written by producer, read by consumer
        alignas(64) std::atomic<size_t> tail_{0}; // written by consumer, read by producer

        alignas(64) std::array<T, CapacityPow2> buffer_{};
};
