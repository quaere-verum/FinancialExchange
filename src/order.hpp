#pragma once
#include "types.hpp"

struct Order {
    Order* next_;
    Order* previous_;
    Id_t client_id_;
    Id_t order_id_;
    Id_t order_handle_;
    Price_t price_;
    Volume_t quantity_;
    Volume_t quantity_remaining_;
    Volume_t quantity_cumulative_;
    bool is_bid_;
};

struct OrderPool {
    Order pool_[MAX_ORDERS];
    Order* next_free_;

    OrderPool() {
        for (size_t i = 0; i < MAX_ORDERS - 1; ++i) {
            pool_[i].next_ = &pool_[i + 1];
            pool_[i].order_handle_ = i;
        }
        pool_[MAX_ORDERS - 1].next_ = nullptr;
        pool_[MAX_ORDERS - 1].order_handle_ = MAX_ORDERS - 1;
        next_free_ = &pool_[0];
    };

    inline Order* allocate() noexcept {
        if (!next_free_) return nullptr;
        Order* order = next_free_;
        next_free_ = next_free_->next_;
        order->next_ = nullptr;
        return order;
    };

    inline void deallocate(Order* order) noexcept {
        order->next_ = next_free_;
        next_free_ = order;
    }

    inline Order* from_handle(Id_t order_handle) noexcept {
        return &pool_[order_handle];
    }
};