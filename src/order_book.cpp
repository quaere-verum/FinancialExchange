#pragma once
#include "order_book.hpp"
#include "time.hpp"

OrderBookSide::OrderBookSide(bool is_bid) : is_bid_(is_bid) {
    best_price_index_ = NUM_BOOK_LEVELS;
    for (size_t i = 0; i < NUM_BOOK_LEVELS; ++i) {
        levels_[i].price_ = MINIMUM_BID + i * TICK_SIZE;
        levels_[i].total_quantity_ = 0;
        levels_[i].first_ = nullptr;
        levels_[i].last_ = nullptr;
    }
}

size_t OrderBookSide::price_to_index(Price_t price) const noexcept {
    assert(price >= MINIMUM_BID && price <= MAXIMUM_ASK);
    return static_cast<size_t>((price - MINIMUM_BID) / TICK_SIZE);
}

Order* OrderBookSide::add_order(Price_t price, Volume_t quantity, Volume_t quantity_remaining, Id_t id, Id_t client_id) noexcept {
    size_t idx = price_to_index(price);
    assert(idx < NUM_BOOK_LEVELS);

    Order* order = pool_.allocate();
    if (!order) return nullptr;

    PriceLevel& level = levels_[idx];
    Order*& first = level.first_;
    Order*& last = level.last_;
    order->client_id_ = client_id;
    order->order_id_ = id;
    order->price_ = price;
    order->quantity_ = quantity;
    order->quantity_remaining_ = quantity_remaining;
    order->quantity_cumulative_ = quantity - quantity_remaining;
    order->next_ = nullptr;
    order->previous_ = last;
    order->is_bid_ = is_bid_;

    if (last) {
        last->next_ = order;
    } else {
        first = order;  
    }
    last = order;
    level.total_quantity_ += quantity;

    if (is_bid_)
        update_best_bid_after_order(idx);
    else
        update_best_ask_after_order(idx);

    return order;
}

void OrderBookSide::update_best_bid_after_order(size_t price_idx) {
    if ((best_price_index_ == NUM_BOOK_LEVELS) || (price_idx > best_price_index_)) {
        best_price_index_ = price_idx;
    }
}

void OrderBookSide::update_best_ask_after_order(size_t price_idx) {
    if ((best_price_index_ == NUM_BOOK_LEVELS) || (price_idx < best_price_index_)) {
        best_price_index_ = price_idx;
    }
}

void OrderBookSide::update_best_bid_after_empty(size_t old_idx) noexcept {
    for (size_t i = old_idx; i-- > 0; ) {
        if (levels_[i].total_quantity_ > 0) {
            best_price_index_ = i;
            return;
        }
    }
    best_price_index_ = NUM_BOOK_LEVELS;
}

void OrderBookSide::update_best_ask_after_empty(size_t old_idx) noexcept {
    for (size_t i = old_idx + 1; i < NUM_BOOK_LEVELS; ++i) {
        if (levels_[i].total_quantity_ > 0) {
            best_price_index_ = i;
            return;
        }
    }
    best_price_index_ = NUM_BOOK_LEVELS;
}

Volume_t OrderBookSide::match_buy(
    Price_t incoming_price, 
    Volume_t incoming_quantity,
    Id_t order_id,
    Id_t client_id
) noexcept {
    Volume_t total_incoming_quantity = incoming_quantity;
    while (incoming_quantity > 0) {
        if (best_price_index_ == NUM_BOOK_LEVELS) break;

        PriceLevel* level = &levels_[best_price_index_];
        if (!(level->price_ <= incoming_price)) break;

        while (incoming_quantity > 0 && level->first_) {
            Order* maker = level->first_;
            Time_t now = utc_now_ns();
            Volume_t trade_quantity = std::min(maker->quantity_remaining_, incoming_quantity);
            maker->quantity_remaining_ -= trade_quantity;
            maker->quantity_cumulative_ += trade_quantity;
            incoming_quantity -= trade_quantity;
            level->total_quantity_ -= trade_quantity;

            callbacks_->on_trade(
                *maker, 
                client_id, 
                order_id, 
                incoming_price, 
                total_incoming_quantity,
                total_incoming_quantity - incoming_quantity,
                trade_quantity,
                now
            );
            callbacks_->on_level_update(Side::BUY, *level, now);

            if (maker->quantity_remaining_ == 0) {
                level->first_ = maker->next_;
                if (!level->first_) {
                    level->last_ = nullptr;
                    update_best_ask_after_empty(best_price_index_);
                }
                pool_.deallocate(maker);
            }
        }
    }
    return incoming_quantity;
}

Volume_t OrderBookSide::match_sell(
    Price_t incoming_price, 
    Volume_t incoming_quantity,
    Id_t order_id,
    Id_t client_id
) noexcept {
    Volume_t total_incoming_quantity = incoming_quantity;
    while (incoming_quantity > 0) {
        if (best_price_index_ == NUM_BOOK_LEVELS) break;

        PriceLevel* level = &levels_[best_price_index_];
        if (!(level->price_ >= incoming_price)) break;

        while (incoming_quantity > 0 && level->first_) {
            Order* maker = level->first_;
            Time_t now = utc_now_ns();
            Volume_t trade_quantity = std::min(maker->quantity_remaining_, incoming_quantity);
            maker->quantity_remaining_ -= trade_quantity;
            maker->quantity_cumulative_ += trade_quantity;
            incoming_quantity -= trade_quantity;
            level->total_quantity_ -= trade_quantity;

            callbacks_->on_trade(
                *maker, 
                client_id, 
                order_id, 
                incoming_price, 
                total_incoming_quantity,
                total_incoming_quantity - incoming_quantity,
                trade_quantity,
                now
            );
            callbacks_->on_level_update(Side::SELL, *level, now);

            if (maker->quantity_remaining_ == 0) {
                level->first_ = maker->next_;
                if (!level->first_) {
                    level->last_ = nullptr;
                    update_best_bid_after_empty(best_price_index_);
                }
                pool_.deallocate(maker);
            }
        }
    }
    return incoming_quantity;
}

void OrderBookSide::print_side(const char* name) const {
    std::cout << "=== " << name << " ===\n";
    for (size_t i = 0; i < NUM_BOOK_LEVELS; ++i) {
        const PriceLevel& level = levels_[i];
        if (level.total_quantity_ == 0) continue;

        std::cout << "Price " << level.price_ << " -> ";
        Order* cur = level.first_;
        while (cur) {
            std::cout << "[client_id=" << cur->client_id_ << ", order_id=" << cur->order_id_
                      << ", qty=" << cur->quantity_ << "] ";
            cur = cur->next_;
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

OrderBook::OrderBook() : bids(true), asks(false), order_id_(0), trade_id_(0) {
    order_index_.reserve(MAX_ORDERS);
    asks.set_callbacks(callbacks_);
    bids.set_callbacks(callbacks_);
}

void OrderBook::set_callbacks(OrderBookCallbacks* callbacks) {
    callbacks_ = callbacks;
    asks.set_callbacks(callbacks);
    bids.set_callbacks(callbacks);
}
void OrderBook::submit_order(Price_t price, Volume_t quantity, bool is_bid, Id_t client_id) {
    if (quantity == 0) return;
    Id_t order_id = order_id_++;
    Volume_t remaining = quantity;

    if (is_bid) {
        remaining = asks.match_buy(price, quantity, order_id, client_id);
        if (remaining > 0) {
            Order* resting_order = bids.add_order(price, quantity, remaining, order_id_, client_id);
            Time_t now = utc_now_ns();
            order_index_[order_id] = resting_order;
            callbacks_->on_order_inserted(0, *resting_order, now);
        }
    } else {
        remaining = bids.match_sell(price, quantity, order_id, client_id);
        if (remaining > 0) {
            Order* resting_order = asks.add_order(price, quantity, remaining, order_id_, client_id);
            Time_t now = utc_now_ns();
            order_index_[order_id] = resting_order;
            callbacks_->on_order_inserted(0, *resting_order, now);
        }
    }
}

void OrderBook::print_book() const {
    bids.print_side("BIDS");
    asks.print_side("ASKS");
}

void OrderBook::cancel_order(Id_t client_id, Id_t order_id) noexcept {
    Time_t now = utc_now_ns();
    auto order_idx = order_index_.find(order_id);
    if (order_idx == order_index_.end()) return;

    Order* order = order_idx->second;
    if (order->client_id_ != client_id) return;

    OrderBookSide& side = order->is_bid_ ? bids : asks;
    size_t idx = side.price_to_index(order->price_);
    PriceLevel& level = side.levels_[idx];

    if (order->previous_)
        order->previous_->next_ = order->next_;
    else
        level.first_ = order->next_;

    if (order->next_)
        order->next_->previous_ = order->previous_;
    else
        level.last_ = order->previous_;

    level.total_quantity_ -= order->quantity_;
    callbacks_->on_level_update(order->is_bid_ ? Side::BUY : Side::SELL, level, now);

    if (!level.first_) {
        if (order->is_bid_)
            side.update_best_bid_after_empty(idx);
        else
            side.update_best_ask_after_empty(idx);
    }
    callbacks_->on_order_cancelled(0, *order, now);
    side.pool_.deallocate(order);
    order_index_.erase(order_idx);
}
