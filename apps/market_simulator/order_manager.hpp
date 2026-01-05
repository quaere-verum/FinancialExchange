#pragma once
#include "types.hpp"
#include <optional>
#include "rng.hpp"
#include "protocol.hpp"


struct OrderInfo {
    Id_t order_id;
    Price_t price;
    Volume_t quantity;
};

class SideOrders {
    public:
        void on_insert_acknowledged(const PayloadConfirmOrderInserted* insert_confirmation) {
            Id_t id = insert_confirmation->exchange_order_id;
            open_orders_[insert_confirmation->exchange_order_id] = {
                id,
                insert_confirmation->price,
                insert_confirmation->leaves_quantity
            };
            id_to_index_[id] = order_ids_.size();
            order_ids_.push_back(id);
        }

        void on_cancel_acknowledged(const PayloadConfirmOrderCancelled* cancel_confirmation) {
            Id_t id = cancel_confirmation->exchange_order_id;
            auto it = id_to_index_.find(id);
            if (it == id_to_index_.end()) {
                return;
            }
            size_t idx = it->second;

            std::swap(order_ids_[idx], order_ids_.back());
            id_to_index_[order_ids_[idx]] = idx;
            order_ids_.pop_back();
            id_to_index_.erase(id);
            open_orders_.erase(id);
        }

        void on_amend_acknowledged(const PayloadConfirmOrderAmended* amend_confirmation) {
            auto it = open_orders_.find(amend_confirmation->exchange_order_id);
            if (it == open_orders_.end()) {
                return;
            }
            it->second.quantity = amend_confirmation->leaves_quantity;
        }

        std::optional<OrderInfo> random_order(RNG* rng) const {
            if (!has_live_orders()) {
                return std::nullopt;
            }
            auto order_idx = rng->uniform_int(0, order_ids_.size() - 1);
            Id_t order_id = order_ids_[order_idx];
            return open_orders_.at(order_id);
        }
        bool has_live_orders() const {return open_orders_.size() > 0;}

    private:
        std::unordered_map<Id_t, OrderInfo> open_orders_;
        std::vector<Id_t> order_ids_;
        std::unordered_map<Id_t, size_t> id_to_index_;
};

class OrderManager {
    public:
        void on_insert_acknowledged(const PayloadConfirmOrderInserted* insert_confirmation) {
            if (insert_confirmation->side == Side::BUY) {
                bids_.on_insert_acknowledged(insert_confirmation);
            } else {
                asks_.on_insert_acknowledged(insert_confirmation);
            }
        }

        void on_cancel_acknowledged(const PayloadConfirmOrderCancelled* cancel_confirmation) {
            if (cancel_confirmation->side == Side::BUY) {
                bids_.on_cancel_acknowledged(cancel_confirmation);
            } else {
                asks_.on_cancel_acknowledged(cancel_confirmation);
            }
        }

        void on_amend_acknowledged(const PayloadConfirmOrderAmended* amend_confirmation) {
            // Need a way to track whether amend request was for bid or ask. Fix later
            bids_.on_amend_acknowledged(amend_confirmation);
            asks_.on_amend_acknowledged(amend_confirmation);
        }

        std::optional<OrderInfo> random_order(RNG* rng, Side side) const {
            if (side == Side::BUY) {
                return bids_.random_order(rng);
            } else {
                return asks_.random_order(rng);
            }
        }

    private:
        SideOrders bids_;
        SideOrders asks_;
};