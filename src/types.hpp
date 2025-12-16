#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using Id_t = uint32_t;
using Price_t = int64_t;
using Volume_t = uint32_t;
using Time_t = uint64_t;
using Seq_t = uint64_t;
using Message_t = uint8_t;

static constexpr size_t MAX_ORDERS = 1'000;
static constexpr Price_t MINIMUM_BID = 1;
static constexpr Price_t MAXIMUM_ASK = 10'000;
static constexpr size_t NUM_BOOK_LEVELS = MAXIMUM_ASK - MINIMUM_BID + 1;
static constexpr size_t ORDER_BOOK_MESSAGE_DEPTH = 10;
static constexpr size_t MAX_TRADES_PER_TICK = 100;

enum class Lifespan : uint8_t {FILL_AND_KILL, GOOD_FOR_DAY};
enum class Side : uint8_t {SELL, BUY};
enum class ErrorType : uint16_t {
    ORDER_BOOK_FULL = 1,
    INVALID_VOLUME = 2,
    ORDER_NOT_FOUND = 3,
    UNAUTHORISED = 4,
    INVALID_PRICE = 5
};

template<typename C, typename T>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& strm, Lifespan span) {
    strm << ((span == Lifespan::FILL_AND_KILL) ? "FAK" : "GFD");
    return strm;
}

template<typename C, typename T>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& strm, Side side) {
    strm << ((side == Side::BUY) ? "Buy" : "Sell");
    return strm;
}

enum class SendMode {ASAP, SOON};

struct IConnection {
    virtual ~IConnection() = default;
    virtual void async_read() = 0;
    virtual void send_message(Message_t message_type, const void* payload, SendMode mode) = 0;
    void send_message(Message_t message_type, const void* payload) {send_message(message_type, payload, SendMode::ASAP);};
    const std::string& get_name() const {return name_;};
    void set_name(std::string name) {name_ = std::move(name);};

    std::function<void(IConnection*)> disconnected;
    std::function<void(IConnection*, Message_t, const uint8_t*)> message_received;

    protected:
        void on_disconnect() {if (disconnected) {disconnected(this);}}
        void on_message_received(Message_t message_type, const uint8_t* data) {
            if (message_received) {message_received(this, message_type, data);}
        }

        std::string name_;
};
