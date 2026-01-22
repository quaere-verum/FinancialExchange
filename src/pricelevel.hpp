#pragma once
#include "types.hpp"
#include "order.hpp"

struct PriceLevel {
    Order* first_;
    Order* last_;
    Volume_t total_quantity_;
    Price_t price_;
    size_t idx_;
};