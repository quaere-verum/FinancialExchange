#pragma once
#include "ring_buffer.hpp"
#include <atomic>
#include <vector>
#include "protocol.hpp"
#include "connectivity.hpp"

class MarketDataDispatcher {
    public:
        explicit MarketDataDispatcher(size_t staging_bytes = 64 * 1024);
        ~MarketDataDispatcher();

        void start();
        void stop();
        bool publish(MessageType type, const void* payload, uint16_t payload_size) noexcept;
        void set_subscribers(std::vector<Connection*>* subscribers) noexcept;

    private:
        void run();
        void flush(const uint8_t* data, size_t len);

    private:
        RingBuffer<1 << 22> queue_;
        std::atomic<bool> running_{false};
        std::thread worker_;

        std::atomic<std::vector<Connection*>*> subscribers_{nullptr};
};
