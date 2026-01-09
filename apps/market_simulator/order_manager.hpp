#pragma once
#include "types.hpp"
#include <optional>
#include <unordered_map>
#include <vector>
#include <map>
#include "rng.hpp"
#include "protocol.hpp"

struct OrderInfo {
    Id_t order_id;
    Price_t price;
    Volume_t quantity;
    Time_t insert_timestamp;
    Side side;
};

class SideOrders {
    public:
        explicit SideOrders(Side side) : side_(side) {}

        void on_partial_fill(const PayloadPartialFill* msg) {
            Id_t id = msg->exchange_order_id;
            auto it = open_orders_.find(id);
            if (it == open_orders_.end()) {
                return;
            } else {
                it->second.quantity = msg->leaves_quantity;
                if (it->second.quantity == 0) {
                    remove_order(id, it->second.price);
                }
            }
        }

        void on_insert_acknowledged(const PayloadConfirmOrderInserted* msg) {
            Id_t id = msg->exchange_order_id;
            OrderInfo info{id, msg->price, msg->leaves_quantity, msg->timestamp, msg->side};
            open_orders_[id] = info;
            id_to_index_[id] = order_ids_.size();
            order_ids_.push_back(id);

            price_levels_[msg->price].push_back(id);
        }

        void on_cancel_acknowledged(const PayloadConfirmOrderCancelled* msg) {
            Id_t id = msg->exchange_order_id;
            auto it = open_orders_.find(id);
            if (it == open_orders_.end()) {
                return;
            }

            Price_t price = it->second.price;

            remove_order(id, price);
        }

        void on_amend_acknowledged(const PayloadConfirmOrderAmended* msg) {
            auto it = open_orders_.find(msg->exchange_order_id);
            if (it == open_orders_.end()) {
                return;
            }

            it->second.quantity = msg->leaves_quantity;
            if (it->second.quantity == 0) {
                remove_order(it->second.order_id, it->second.price);
            }
        }

        std::optional<OrderInfo> random_order(RNG* rng) const {
            if (order_ids_.empty()) {
                return std::nullopt;
            }
            size_t idx = rng->uniform_int(0, order_ids_.size() - 1);
            return open_orders_.at(order_ids_[idx]);
        }

        bool has_live_orders() const {
            return !open_orders_.empty();
        }

        double depth_ahead(const OrderInfo& order) const {
            double depth = 0.0;

            if (side_ == Side::BUY) {
                for (auto it = price_levels_.rbegin(); it != price_levels_.rend(); ++it) {
                    if (it->first < order.price) break;
                    for (Id_t id : it->second) {
                        if (id == order.order_id) break;
                        depth += open_orders_.at(id).quantity;
                    }
                }
            } else {
                for (auto it = price_levels_.begin(); it != price_levels_.end(); ++it) {
                    if (it->first > order.price) break;
                    for (Id_t id : it->second) {
                        if (id == order.order_id) break;
                        depth += open_orders_.at(id).quantity;
                    }
                }
            }

            return depth;
        }

    private:
        void remove_order(Id_t order_id, Price_t price) {
            // Remove from random sampling structures
            size_t idx = id_to_index_[order_id];
            std::swap(order_ids_[idx], order_ids_.back());
            id_to_index_[order_ids_[idx]] = idx;
            order_ids_.pop_back();
            id_to_index_.erase(order_id);

            // Remove from price level
            auto& vec = price_levels_[price];
            auto it = std::find(vec.begin(), vec.end(), order_id);
            if (it != vec.end()) {
                vec.erase(it);
            }

            if (vec.empty()) {
                price_levels_.erase(price);
            }

            open_orders_.erase(order_id);
        }

        Side side_;

        std::unordered_map<Id_t, OrderInfo> open_orders_;
        std::vector<Id_t> order_ids_;
        std::unordered_map<Id_t, size_t> id_to_index_;

        std::map<Price_t, std::vector<Id_t>> price_levels_;
};

class OrderManager {
public:
    OrderManager()
        : bids_(Side::BUY),
          asks_(Side::SELL) {}

    void on_insert_acknowledged(const PayloadConfirmOrderInserted* msg) {
        if (msg->side == Side::BUY) {
            bids_.on_insert_acknowledged(msg);
        } else {
            asks_.on_insert_acknowledged(msg);
        }
    }

    void on_cancel_acknowledged(const PayloadConfirmOrderCancelled* msg) {
        if (msg->side == Side::BUY) {
            bids_.on_cancel_acknowledged(msg);
        } else {
            asks_.on_cancel_acknowledged(msg);
        }
    }

    void on_amend_acknowledged(const PayloadConfirmOrderAmended* msg) {
        // Need to adjust to determine side beforehand
        bids_.on_amend_acknowledged(msg);
        asks_.on_amend_acknowledged(msg);
    }

    void on_partial_fill(const PayloadPartialFill* msg) {
        // Need to adjust to determine side beforehand
        bids_.on_partial_fill(msg);
        asks_.on_partial_fill(msg);
    }

    const SideOrders& bids() const { return bids_; }
    const SideOrders& asks() const { return asks_; }

    std::optional<OrderInfo> random_order(RNG* rng, Side side) const {
        return (side == Side::BUY)
            ? bids_.random_order(rng)
            : asks_.random_order(rng);
    }

    inline double depth_ahead(const OrderInfo& order) const {
        if (order.side == Side::BUY) {
            return bids_.depth_ahead(order);
        } else {
            return asks_.depth_ahead(order);
        }
    }

    double distance_from_touch(const OrderInfo& order, const PriceState& price_state) const {
        auto best = order.side == Side::BUY ? price_state.best_bid : price_state.best_ask;
        if (!best) {
            return 0.0;
        }
        return std::abs(static_cast<double>(order.price) - static_cast<double>(*best));
    }

    double order_age(const OrderInfo& order, Time_t now) const {
        return static_cast<double>(now - order.insert_timestamp) * 1e-9;
    }

private:
    SideOrders bids_;
    SideOrders asks_;
};
