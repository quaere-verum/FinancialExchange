#pragma once
#include "types.hpp"
#include "order.hpp"
#include "pricelevel.hpp"

struct OrderBookCallbacks {
    virtual ~OrderBookCallbacks() = default;
    virtual void on_trade(
        const Order& maker_order,
        Id_t taker_client_id,
        Id_t taker_order_id,
        Price_t price,
        Volume_t taker_total_quantity,
        Volume_t taker_cumulative_quantity,
        Volume_t traded_quantity,
        Time_t timestamp
    ) = 0;
    virtual void on_order_inserted(Id_t client_request_id, const Order& order, Time_t timestamp) = 0;
    virtual void on_order_cancelled(Id_t client_request_id, const Order& order, Time_t timestamp) = 0;
    virtual void on_order_amended(Id_t client_request_id, Volume_t quantity_old, const Order& order, Time_t timestamp) = 0;
    virtual void on_level_update(Side side, PriceLevel const& level, Time_t timestamp) = 0;
    virtual void on_error(Id_t client_id, Id_t client_request_id, uint16_t code, std::string_view message, Time_t timestamp) = 0;
};