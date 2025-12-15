#pragma once
#include "types.hpp"
#include "order.hpp"

struct PriceLevel {
    size_t idx_;
    Price_t price_;
    Volume_t total_quantity_;
    Order* first_;
    Order* last_;
};