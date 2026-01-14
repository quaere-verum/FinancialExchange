#pragma once
#include <array>
#include "order_book.hpp"
#include "time.hpp"
#include "logging.hpp"

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")

inline void _debug_check_level_invariant(const PriceLevel& level) {
#ifndef NDEBUG
    int64_t sum_remaining = 0;
    for (auto* o = level.first_; o; o = o->next_) {
        sum_remaining += o->quantity_remaining_;
    }
    assert(sum_remaining == level.total_quantity_);
#endif
}

inline void _debug_check_level_integrity(const OrderBook& order_book) {
#ifndef NDEBUG
    if (order_book.bids.best_price_index_ == NUM_BOOK_LEVELS) {
        return;
    }
    if (order_book.asks.best_price_index_ == NUM_BOOK_LEVELS) {
        return;
    }
    const Price_t best_bid = order_book.bids.levels_[order_book.bids.best_price_index_].price_;
    const Price_t best_ask = order_book.asks.levels_[order_book.asks.best_price_index_].price_;
    assert(best_bid < best_ask);
    return;
#endif
}

OrderBookSide::OrderBookSide(bool is_bid) : is_bid_(is_bid) {
    best_price_index_ = NUM_BOOK_LEVELS;
    for (size_t i = 0; i < NUM_BOOK_LEVELS; ++i) {
        levels_[i].idx_ = i;
        levels_[i].price_ = MINIMUM_BID + i;
        levels_[i].total_quantity_ = 0;
        levels_[i].first_ = nullptr;
        levels_[i].last_ = nullptr;
    }
}

size_t OrderBookSide::price_to_index(Price_t price) const noexcept {
    return static_cast<size_t>((price - MINIMUM_BID));
}

Order* OrderBookSide::add_order(
    Price_t price, 
    Volume_t quantity, 
    Volume_t quantity_remaining, 
    Id_t order_id, 
    Id_t client_id,
    Id_t client_request_id
) noexcept {
    Time_t now = utc_now_ns();
    size_t idx = price_to_index(price);
    Order* order = pool_.allocate();
    if (!order) {
        callbacks_->on_error(
            client_id, 
            client_request_id, 
            static_cast<uint16_t>(ErrorType::ORDER_BOOK_FULL),
            "Order book is full.",
            now
        );
        return nullptr;
    }

    PriceLevel& level = levels_[idx];

    _debug_check_level_invariant(level);

    Order*& first = level.first_;
    Order*& last = level.last_;
    order->client_id_ = client_id;
    order->order_id_ = order_id;
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
    level.total_quantity_ += quantity_remaining;
    callbacks_->on_level_update(is_bid_ ? Side::BUY : Side::SELL, level, now);
    if (is_bid_)
        update_best_bid_after_order(idx);
    else
        update_best_ask_after_order(idx);

    _debug_check_level_invariant(level);

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

void OrderBookSide::update_best_bid_after_empty() noexcept {
    size_t old_idx = best_price_index_;
    for (size_t i = old_idx; i-- > 0; ) {
        if (levels_[i].total_quantity_ > 0) {
            best_price_index_ = i;
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Updating best bid after empty to p=" << levels_[i].price_ << ".";
            return;
        }
    }
    best_price_index_ = NUM_BOOK_LEVELS;
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Bid side is empty.";
}

void OrderBookSide::update_best_ask_after_empty() noexcept {
    size_t old_idx = best_price_index_;
    for (size_t i = old_idx + 1; i < NUM_BOOK_LEVELS; ++i) {
        if (levels_[i].total_quantity_ > 0) {
            best_price_index_ = i;
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Updating best ask after empty to p=" << levels_[i].price_ << ".";
            return;
        }
    }
    best_price_index_ = NUM_BOOK_LEVELS;
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Ask side is empty.";
}

template <typename PriceCrossFn, typename BestPriceFn>
Volume_t OrderBookSide::match_loop(
    Price_t incoming_price,
    Volume_t incoming_quantity,
    Id_t order_id,
    Id_t client_id,
    Side maker_side,
    PriceCrossFn crosses,
    BestPriceFn advance_best,
    std::vector<Id_t>& filled_order_ids
) noexcept {
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Order from " << client_id << " with id=" << order_id 
    << ", qty=" << incoming_quantity << ", p=" << incoming_price << " entering matching process.";
    Time_t now = utc_now_ns();
    Volume_t total_incoming_quantity = incoming_quantity;

    while (incoming_quantity > 0) {


        if (best_price_index_ == NUM_BOOK_LEVELS)
            break;

        PriceLevel* level = &levels_[best_price_index_];

        _debug_check_level_invariant(*level);

        if (!crosses(level->price_, incoming_price))
            break;

        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Order from " << client_id << " with id=" << order_id 
        << ", qty=" << incoming_quantity << ", p=" << incoming_price << " being matched at level p=" << level->price_ <<
        ", qty=" << level->total_quantity_ << ".";

        while (incoming_quantity > 0 && level->first_) {
            Order* maker = level->first_;

            Volume_t trade_quantity = std::min(maker->quantity_remaining_, incoming_quantity);

            maker->quantity_remaining_ -= trade_quantity;
            maker->quantity_cumulative_ += trade_quantity;
            incoming_quantity -= trade_quantity;
            level->total_quantity_ -= trade_quantity;

            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBookSide] Order from " << client_id << " with ID " << order_id << " matched.";

            callbacks_->on_trade(
                *maker,
                client_id,
                order_id,
                maker->price_,
                total_incoming_quantity,
                total_incoming_quantity - incoming_quantity,
                trade_quantity,
                now
            );
            callbacks_->on_level_update(maker_side, *level, now);

            if (maker->quantity_remaining_ == 0) {
                filled_order_ids.push_back(maker->order_id_);
                Order* next = maker->next_;
                level->first_ = next;
                if (next) {
                    next->previous_ = nullptr;
                } else {
                    level->last_ = nullptr;
                    advance_best();
                }
                pool_.deallocate(maker);
            }
            _debug_check_level_invariant(*level);
        }
    }
    return incoming_quantity;
}


Volume_t OrderBookSide::match_buy(
    Price_t incoming_price, 
    Volume_t incoming_quantity,
    Id_t order_id,
    Id_t client_id,
    std::vector<Id_t>& filled_order_ids
) noexcept {
    return match_loop(
        incoming_price,
        incoming_quantity,
        order_id,
        client_id,
        Side::SELL,
        [](Price_t level_price, Price_t incoming) {return level_price <= incoming;},
        [this]() {update_best_ask_after_empty();},
        filled_order_ids
    );
}

Volume_t OrderBookSide::match_sell(
    Price_t incoming_price, 
    Volume_t incoming_quantity,
    Id_t order_id,
    Id_t client_id,
    std::vector<Id_t>& filled_order_ids
) noexcept {
    return match_loop(
        incoming_price,
        incoming_quantity,
        order_id,
        client_id,
        Side::BUY,
        [](Price_t level_price, Price_t incoming) {return level_price >= incoming;},
        [this]() {update_best_bid_after_empty();},
        filled_order_ids
    );
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
    filled_order_ids_.reserve(MAX_TRADES_PER_TICK); // TODO: actually enforce max trades per tick
    asks.set_callbacks(callbacks_);
    bids.set_callbacks(callbacks_);
}

void OrderBook::set_callbacks(OrderBookCallbacks* callbacks) {
    callbacks_ = callbacks;
    asks.set_callbacks(callbacks);
    bids.set_callbacks(callbacks);
}

void OrderBook::submit_order(Price_t price, Volume_t quantity, bool is_bid, Id_t client_id, Id_t client_request_id) {
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBook] Order from " << client_id << " with request ID " << client_request_id << " submitted into order book.";
    Time_t now = utc_now_ns();
    if (quantity == 0) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::INVALID_VOLUME),
            "Invalid order size.",
            now
        );
        return;
    }
    if (price < MINIMUM_BID || price > MAXIMUM_ASK) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::INVALID_PRICE),
            "Invalid price.",
            now
        );
        return;
    }
    Id_t order_id = order_id_++;
    Volume_t remaining = quantity;
    filled_order_ids_.clear();

    if (is_bid) {
        remaining = asks.match_buy(price, quantity, order_id, client_id, filled_order_ids_);
        if (remaining > 0) {
            Order* resting_order = bids.add_order(price, quantity, remaining, order_id, client_id, client_request_id);
            order_index_[order_id] = resting_order;
            callbacks_->on_order_inserted(0, *resting_order, now);
        }
    } else {
        remaining = bids.match_sell(price, quantity, order_id, client_id, filled_order_ids_);
        if (remaining > 0) {
            Order* resting_order = asks.add_order(price, quantity, remaining, order_id, client_id, client_request_id);
            order_index_[order_id] = resting_order;
            callbacks_->on_order_inserted(client_request_id, *resting_order, now);
        }
    }
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[OrderBook] Order from " << client_id << " with request ID " << client_request_id << " matched against resting orders.";
    for (Id_t order_idx : filled_order_ids_) {
        order_index_.erase(order_idx);
    }
    _debug_check_level_integrity(*this);
}

void OrderBook::print_book() const {
    bids.print_side("BIDS");
    asks.print_side("ASKS");
}

void OrderBook::cancel_order(Id_t client_id, Id_t client_request_id, Id_t order_id) noexcept {
    Time_t now = utc_now_ns();
    auto order_idx = order_index_.find(order_id);
    if (order_idx == order_index_.end()) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::ORDER_NOT_FOUND),
            "Order ID not found.",
            now
        );
        return;
    } 

    Order* order = order_idx->second;
    if (order->client_id_ != client_id) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::UNAUTHORISED),
            "Unauthorised request.",
            now
        );
        return;
    }

    OrderBookSide& side = order->is_bid_ ? bids : asks;
    size_t idx = side.price_to_index(order->price_);
    PriceLevel& level = side.levels_[idx];

    _debug_check_level_invariant(level);

    level.total_quantity_ -= order->quantity_remaining_;

    Order order_snapshot = *order;
    remove_order(order_idx->first, order, side, level);

    if (!level.first_ && side.best_price_index_ == level.idx_) {
        if (order->is_bid_)
            side.update_best_bid_after_empty();
        else
            side.update_best_ask_after_empty();
    }

    callbacks_->on_level_update(order_snapshot.is_bid_ ? Side::BUY : Side::SELL, level, now);
    callbacks_->on_order_cancelled(client_request_id, order_snapshot, now);
    
    _debug_check_level_invariant(level);
}

void OrderBook::amend_order(Id_t client_id, Id_t client_request_id, Id_t order_id, Volume_t quantity_new) noexcept {
    Time_t now = utc_now_ns();
    auto order_idx = order_index_.find(order_id);
    if (order_idx == order_index_.end()) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::ORDER_NOT_FOUND),
            "Order ID not found.",
            now
        );
        return;
    } 

    Order* order = order_idx->second;
    if (order->client_id_ != client_id) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::UNAUTHORISED),
            "Unauthorised request.",
            now
        );
        return;
    }

    if (quantity_new < order->quantity_cumulative_) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::INVALID_VOLUME),
            "Invalid order size.",
            now
        );
        return;
    }

    Volume_t quantity_old_total = order->quantity_;
    Volume_t quantity_old_remaining = order->quantity_remaining_;

    Volume_t quantity_new_total = quantity_new;
    Volume_t quantity_new_remaining = quantity_new_total - order->quantity_cumulative_;

    if (quantity_old_remaining < quantity_new_remaining) {
        callbacks_->on_error(
            client_id, 
            client_request_id,
            static_cast<uint16_t>(ErrorType::INVALID_VOLUME),
            "Invalid order size.",
            now
        );
        return;
    }

    Volume_t delta = quantity_old_remaining - quantity_new_remaining;

    if (quantity_new_remaining == quantity_old_remaining) {
        callbacks_->on_order_amended(
            client_request_id,
            quantity_old_total,
            *order,
            now
        );
        return;
    }

    OrderBookSide& side = order->is_bid_ ? bids : asks;
    size_t idx = side.price_to_index(order->price_);
    PriceLevel& level = side.levels_[idx];

    _debug_check_level_invariant(level);

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "(Pre amend update) level_qty=" << level.total_quantity_ << ", old_remaining_qty=" << order->quantity_remaining_ 
    << ", new_remaining_qty=" << quantity_new_remaining << "\n";

    order->quantity_ = quantity_new_total;
    order->quantity_remaining_ = quantity_new_remaining;
    level.total_quantity_ -= delta;

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "(Post amend update) level_qty=" << level.total_quantity_ << ", delta=" << delta << "\n";

    Order order_snapshot = *order;

    if (level.total_quantity_ == 0) {
        order_snapshot.is_bid_ ? side.update_best_bid_after_empty() : side.update_best_ask_after_empty();
    }

    callbacks_->on_order_amended(client_request_id, quantity_old_total, order_snapshot, now);
    callbacks_->on_level_update(order_snapshot.is_bid_ ? Side::BUY : Side::SELL, level, now);
    if (quantity_new_remaining == 0) {
        remove_order(order_idx->first, order, side, level);
    }
    
    _debug_check_level_invariant(level);
}

void OrderBook::remove_order(Id_t order_idx, Order* order, OrderBookSide& side, PriceLevel& level) {
    if (order->previous_) {
        order->previous_->next_ = order->next_;
    } else {
        level.first_ = order->next_;
    }
    if (order->next_) {
        order->next_->previous_ = order->previous_;
    } else {
        level.last_ = order->previous_;
    }
    side.pool_.deallocate(order);
    order_index_.erase(order_idx);
}

void OrderBook::build_snapshot(
    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_volumes,
    std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_prices,
    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_volumes,
    std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_prices
) {
    bid_volumes.fill(0);
    bid_prices.fill(0);
    ask_volumes.fill(0);
    ask_prices.fill(0);

    {
        size_t depth = 0;
        size_t idx = bids.best_price_index_;

        while (idx < NUM_BOOK_LEVELS && depth < ORDER_BOOK_MESSAGE_DEPTH) {
            const PriceLevel& level = bids.levels_[idx];

            if (level.total_quantity_ > 0) {
                bid_prices[depth]  = level.price_;
                bid_volumes[depth] = level.total_quantity_;
                ++depth;
            }

            if (idx == 0) break;
            --idx;
        }
    }

    {
        size_t depth = 0;
        size_t idx = asks.best_price_index_;

        while (idx < NUM_BOOK_LEVELS && depth < ORDER_BOOK_MESSAGE_DEPTH) {
            const PriceLevel& level = asks.levels_[idx];

            if (level.total_quantity_ > 0) {
                ask_prices[depth]  = level.price_;
                ask_volumes[depth] = level.total_quantity_;
                ++depth;
            }

            ++idx;
        }
    }
}
