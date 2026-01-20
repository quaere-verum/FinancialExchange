#include "market_data_dispatch.hpp"

MarketDataDispatcher::MarketDataDispatcher(size_t) {}

MarketDataDispatcher::~MarketDataDispatcher() {
    stop();
}

void MarketDataDispatcher::start() {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&MarketDataDispatcher::run, this);
}

void MarketDataDispatcher::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;

    if (worker_.joinable())
        worker_.join();
}

void MarketDataDispatcher::set_subscribers(std::vector<Connection*>* subs) noexcept {
    subscribers_.store(subs, std::memory_order_release);
}

bool MarketDataDispatcher::publish(MessageType type, const void* payload, uint16_t payload_size) noexcept {
    MessageHeader header{ type, payload_size };

    uint8_t buffer[sizeof(MessageHeader) + MAX_PAYLOAD_SIZE];

    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), payload, payload_size);

    const size_t total = sizeof(header) + payload_size;

    return queue_.try_push(buffer, total);
}

void MarketDataDispatcher::run() {
    alignas(64) uint8_t staging[64 * 1024];
    size_t offset = 0;

    while (running_.load(std::memory_order_acquire)) {
        auto [ptr, len] = queue_.peek();

        if (len == 0) {
            if (offset > 0) {
                flush(staging, offset);
                offset = 0;
            }
            _mm_pause();
            continue;
        }

        const size_t to_copy = std::min(len, sizeof(staging) - offset);

        std::memcpy(staging + offset, ptr, to_copy);
        offset += to_copy;

        queue_.advance_read_index(to_copy);

        if (offset == sizeof(staging)) {
            flush(staging, offset);
            offset = 0;
        }
    }

    if (offset > 0) {
        flush(staging, offset);
    }
}

void MarketDataDispatcher::flush(const uint8_t* data, size_t len) {
    auto* subs = subscribers_.load(std::memory_order_acquire);
    if (!subs) return;

    for (Connection* c : *subs) {
        if (c) {
            c->send_raw(data, len);
        }
    }
}
