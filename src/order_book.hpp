#pragma once

#include <cstdint>
#include <cassert>
#include <algorithm>
#include <vector>
#include <iostream>
#include <iomanip>
#include "types.hpp"
#include "order.hpp"
#include "pricelevel.hpp"
#include "callbacks.hpp"

struct OrderBookSide {
    PriceLevel levels_[NUM_BOOK_LEVELS];
    OrderPool pool_;
    bool is_bid_;
    size_t best_price_index_;

    OrderBookSide(bool is_bid);

    inline size_t price_to_index(Price_t price) const noexcept;
    Volume_t match_buy(Price_t incoming_price, Volume_t incoming_quantity, Id_t order_id, Id_t client_id, std::vector<Id_t>& filled_order_ids) noexcept;
    Volume_t match_sell(Price_t incoming_price, Volume_t incoming_quantity, Id_t order_id, Id_t client_id, std::vector<Id_t>& filled_orders_ids) noexcept;
    void print_side(const char* name) const;
    Order* add_order(Price_t price, Volume_t quantity, Volume_t quantity_remaining, Id_t id, Id_t client_id) noexcept;
    void update_best_bid_after_order(size_t price_idx);
    void update_best_ask_after_order(size_t price_idx);
    void update_best_bid_after_empty() noexcept;
    void update_best_ask_after_empty() noexcept;
    void set_callbacks(OrderBookCallbacks* callbacks) {callbacks_ = callbacks;}



    private:
        template <typename PriceCrossFn, typename BestPriceFn>
        Volume_t match_loop(
            Price_t incoming_price,
            Volume_t incoming_quantity,
            Id_t order_id,
            Id_t client_id,
            Side maker_side,
            PriceCrossFn crosses,
            BestPriceFn advance_best,
            std::vector<Id_t>& filled_order_ids
        ) noexcept;
        OrderBookCallbacks* callbacks_;
};

struct OrderBook {
    OrderBookSide bids;
    OrderBookSide asks;

    OrderBook();

    void submit_order(Price_t price, Volume_t quantity, bool is_bid, Id_t client_id);
    void print_book() const;
    void cancel_order(Id_t client_id, Id_t order_id) noexcept;
    void amend_order(Id_t client_id, Id_t order_id, Volume_t quantity_new) noexcept;
    void set_callbacks(OrderBookCallbacks* callbacks);
    void remove_order(Id_t order_idx, Order* order, OrderBookSide& side, PriceLevel& level);
    void build_snapshot(
        std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_volumes,
        std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_prices,
        std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_volumes,
        std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_prices
    );

    private:
        Id_t order_id_;
        Id_t trade_id_;
        std::unordered_map<Id_t, Order*> order_index_;
        OrderBookCallbacks* callbacks_ = nullptr;
        std::vector<Id_t> filled_order_ids_;
};

