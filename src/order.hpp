#pragma once
#include "types.hpp"

struct Order {
    Id_t client_id_;
    Id_t order_id_;
    Price_t price_;
    Volume_t quantity_;
    Volume_t quantity_remaining_;
    Volume_t quantity_cumulative_;
    Order* next_;
    Order* previous_;
    bool is_bid_;
};

struct OrderPool {
    Order pool_[MAX_ORDERS];
    Order* next_free_;

    OrderPool() {
        for (size_t i = 0; i < MAX_ORDERS - 1; ++i) {
            pool_[i].next_ = &pool_[i + 1];
        }
        pool_[MAX_ORDERS - 1].next_ = nullptr;
        next_free_ = &pool_[0];
    };
    Order* allocate() noexcept {
        if (!next_free_) return nullptr;
        Order* order = next_free_;
        next_free_ = next_free_->next_;
        order->next_ = nullptr;
        return order;
    };
    void deallocate(Order* order) noexcept {
        order->next_ = next_free_;
        next_free_ = order;
    }
};