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
        , timer_(io)
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
                    reschedule_next_expiry();
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

        void update_cancel_rate(double lambda_cancel) {
            boost::asio::post(
                strand_,
                [this, lambda_cancel] {
                    advance_hazard_to_now();
                    lambda_cancel_ = lambda_cancel;
                    reschedule_next_expiry();
                }
            );
        }

        size_t open_order_count() const {
            return active_orders_.size();
        }

        const double cumulative_hazard() const {return cumulative_hazard_;}

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

        void advance_hazard_to_now() {
            auto now = std::chrono::steady_clock::now();

            if (last_update_time_.time_since_epoch().count() != 0 && lambda_cancel_ > 0.0) {
                double dt = std::chrono::duration<double>(now - last_update_time_).count();
                cumulative_hazard_ += lambda_cancel_ * dt;
            }

            last_update_time_ = now;
        }

        void reschedule_next_expiry() {
            timer_.cancel();

            if (expiry_queue_.empty() || lambda_cancel_ <= 0.0) {
                return;
            }

            const auto& next = expiry_queue_.top();

            double remaining_hazard = next.hazard_threshold - cumulative_hazard_;

            if (remaining_hazard <= 0.0) {
                boost::asio::post(
                    strand_,
                    [this] {
                        fire_next_expiry(boost::system::error_code{});
                    }
                );
                return;
            }


            double dt = remaining_hazard / lambda_cancel_;

            timer_.expires_after(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(dt)));

            timer_.async_wait(
                boost::asio::bind_executor(
                    strand_,
                    [this](const boost::system::error_code& ec) {
                        fire_next_expiry(ec);
                    }
                )
            );
        }

        void fire_next_expiry(const boost::system::error_code& ec) {
            if (ec == boost::asio::error::operation_aborted)
                return;

            advance_hazard_to_now();

            if (expiry_queue_.empty())
                return;

            auto entry = expiry_queue_.top();
            expiry_queue_.pop();

            if (!active_orders_.erase(entry.exchange_order_id)) {
                reschedule_next_expiry();
                return;
            }

            #ifndef NDEBUG
                std::cout << "[OrderManager] Cancelling order "
                        << entry.exchange_order_id
                        << " at cumulative_hazard=" << cumulative_hazard_
                        << "\n";
            #endif

            const Id_t client_id = client_request_id_++;
            connection_.send_message(
                static_cast<Message_t>(MessageType::CANCEL_ORDER),
                &make_cancel_order(client_id, entry.exchange_order_id),
                SendMode::ASAP
            );

            reschedule_next_expiry();
        }

    private:
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        boost::asio::steady_timer timer_;
        Connection& connection_;
        std::atomic<Id_t>& client_request_id_;

        double cumulative_hazard_ = 0.0;
        double lambda_cancel_ = 0.0;
        std::chrono::steady_clock::time_point last_update_time_;

        std::priority_queue<
            HazardEntry,
            std::vector<HazardEntry>,
            CompareHazard
        > expiry_queue_;

        std::unordered_set<Id_t> active_orders_;
        std::unordered_map<Id_t, double> pending_inserts_;
};
