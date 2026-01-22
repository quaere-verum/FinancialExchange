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

static constexpr size_t MAX_ORDERS = 100'000;
static constexpr Price_t MINIMUM_BID = 1;
static constexpr Price_t MAXIMUM_ASK = 10'000;
static constexpr size_t NUM_BOOK_LEVELS = MAXIMUM_ASK - MINIMUM_BID + 1;
static constexpr size_t ORDER_BOOK_MESSAGE_DEPTH = 10;
static constexpr size_t MAX_TRADES_PER_TICK = 100;
constexpr size_t ERROR_TEXT_LEN = 32;

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

