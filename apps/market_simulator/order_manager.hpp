#pragma once

#include <boost/asio.hpp>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <atomic>

#include "types.hpp"
#include "protocol.hpp"

class OrderManager {
    public:
        OrderManager(
            boost::asio::io_context& io,
            Connection& connection,
            std::atomic<Id_t>& request_id
        )
        : strand_(boost::asio::make_strand(io))
        , connection_(connection)
        , client_request_id_(request_id)
        {}

        void register_pending_insert(Id_t client_request_id, double hazard_threshold) {
            boost::asio::post(
                strand_,
                [this, client_request_id, hazard_threshold] {
                    pending_inserts_[client_request_id] = hazard_threshold;
                }
            );
        }

        void on_insert_acknowledged(const PayloadConfirmOrderInserted* msg) {
            const Id_t client_id = msg->client_request_id;
            const Id_t exchange_id = msg->exchange_order_id;

            boost::asio::post(
                strand_,
                [this, client_id, exchange_id] {
                    auto it = pending_inserts_.find(client_id);
                    if (it == pending_inserts_.end()) {
                        return;
                    }

                    const double hazard_threshold = it->second;
                    pending_inserts_.erase(it);

                    active_orders_.insert(exchange_id);
                    expiry_queue_.push({hazard_threshold, exchange_id});
                }
            );
        }

        void on_partial_fill(const PayloadPartialFill* msg) {
            if (msg->leaves_quantity > 0) {
                return;
            }

            boost::asio::post(
                strand_,
                [this, exchange_id = msg->exchange_order_id] {
                    active_orders_.erase(exchange_id);
                }
            );
        }

        void on_hazard_advanced(double cumulative_hazard) {
            boost::asio::post(
                strand_,
                [this, cumulative_hazard] {
                    handle_expiries(cumulative_hazard);
                }
            );
        }

        size_t open_order_count() const {
            return active_orders_.size();
        }

    private:
        struct HazardEntry {
            double hazard_threshold;
            Id_t   exchange_order_id;
        };

        struct CompareHazard {
            bool operator()(const HazardEntry& a, const HazardEntry& b) const {
                return a.hazard_threshold > b.hazard_threshold;
            }
        };

        void handle_expiries(double cumulative_hazard) {
            while (!expiry_queue_.empty() && expiry_queue_.top().hazard_threshold <= cumulative_hazard) {
                const auto entry = expiry_queue_.top();
                expiry_queue_.pop();

                #ifndef NDEBUG
                std::cout << "[OrderManager] Expiry firing for order " << entry.exchange_order_id
                    << " at hazard=" << entry.hazard_threshold
                    << ", cumulative_hazard=" << cumulative_hazard << "\n";
                #endif
                if (!active_orders_.erase(entry.exchange_order_id)) {
                    continue;
                }

                const Id_t client_id = client_request_id_++;
                connection_.send_message(
                    static_cast<Message_t>(MessageType::CANCEL_ORDER),
                    &make_cancel_order(client_id, entry.exchange_order_id),
                    SendMode::ASAP
                );
            }
        }

    private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        Connection& connection_;
        std::atomic<Id_t>& client_request_id_;

        std::priority_queue<
            HazardEntry,
            std::vector<HazardEntry>,
            CompareHazard
        > expiry_queue_;

        std::unordered_set<Id_t> active_orders_;
        std::unordered_map<Id_t, double> pending_inserts_;
};
