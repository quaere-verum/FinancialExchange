#pragma once

#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <chrono>

#include <boost/asio.hpp>

#include "types.hpp"
#include "protocol.hpp"
#include "rng.hpp"
#include "connectivity.hpp"
#include "market_dynamics.hpp"
#include "order_manager.hpp"
#include "state.hpp"
#include "shadow_order_book.hpp"

template <size_t N>
class MarketSimulator {
    public:
        MarketSimulator(
            boost::asio::io_context& context,
            tcp::socket socket,
            std::unique_ptr<RNG> rng,
            const std::array<Price_t, N>& liquidity_bucket_bounds
        )
            : context_(context)
            , rng_(std::move(rng))
            , on_message_strand_(context_.get_executor())
            , connection_(context, std::move(socket), 0, &on_message_strand_)
            , state_(liquidity_bucket_bounds)
            , request_id_(0) {
                connection_.message_received = [this](IConnection* from, Message_t type, const uint8_t* data) {
                    this->on_message(
                        static_cast<Connection*>(from),
                        type,
                        data
                    );
                };
                cp_.resize(3);
            }

        ~MarketSimulator() {stop();}

        void start() {
            running_ = true;
            schedule_next_event();
        }

        void stop() {running_ = false;}

        void set_insert_intensity(double lambda) noexcept {lambda_insert_ = lambda;}

        void set_cancel_intensity(double lambda) noexcept {lambda_cancel_ = lambda;}

        void set_amend_intensity(double lambda) noexcept {lambda_amend_ = lambda;}

        double lambda() const noexcept {return lambda_insert_ + lambda_cancel_ + lambda_amend_;}

    private:
        void schedule_next_event() {
            const double lambda_tot = lambda();
            if (lambda_tot <= 0.0 || !running_) {
                return;
            }

            const double dt = rng_->exponential(lambda_tot);

            using steady_duration = boost::asio::steady_timer::duration;

            auto timer = std::make_shared<boost::asio::steady_timer>(
                on_message_strand_,
                std::chrono::duration_cast<steady_duration>(
                    std::chrono::duration<double>(dt)
                )
            );

            timer->async_wait(
                [this, timer, dt](const boost::system::error_code& ec) {
                    if (!ec && running_) {
                        state_.sync_with_book(shadow_order_book_, dt);
                        switch (sample_event_type()) {
                            case EventType::INSERT_ORDER: generate_insert(); break;
                            case EventType::CANCEL_ORDER: generate_cancel(); break;
                            case EventType::AMEND_ORDER:  generate_amend();  break;
                        }

                        schedule_next_event();
                    }
                }
            );
        }


        void on_message(
            Connection* from,
            Message_t message_type,
            const uint8_t* payload
        ) {

            switch (static_cast<MessageType>(message_type)) {
                case MessageType::ORDER_BOOK_SNAPSHOT: {
                    const PayloadOrderBookSnapshot* snapshot = reinterpret_cast<const PayloadOrderBookSnapshot*>(payload);
                    shadow_order_book_.on_order_book_snapshot(snapshot);
                    break;
                }
                case MessageType::PRICE_LEVEL_UPDATE: {
                    const PayloadPriceLevelUpdate* update = reinterpret_cast<const PayloadPriceLevelUpdate*>(payload);
                    shadow_order_book_.on_price_level_update(update);
                    break;
                }
                case MessageType::TRADE_EVENT: {
                    const PayloadTradeEvent* trade = reinterpret_cast<const PayloadTradeEvent*>(payload);
                    state_.on_trade(trade);
                    break;
                } case MessageType::CONFIRM_ORDER_INSERTED: {
                    const PayloadConfirmOrderInserted* insert_confirmation = reinterpret_cast<const PayloadConfirmOrderInserted*>(payload);
                    order_manager_.on_insert_acknowledged(insert_confirmation);
                    break;
                }
                case MessageType::CONFIRM_ORDER_CANCELLED: {
                    const PayloadConfirmOrderCancelled* cancel_confirmation = reinterpret_cast<const PayloadConfirmOrderCancelled*>(payload);
                    order_manager_.on_cancel_acknowledged(cancel_confirmation);
                    break;
                }
                case MessageType::ORDER_AMENDED_EVENT: {
                    const PayloadConfirmOrderAmended* amend_confirmation = reinterpret_cast<const PayloadConfirmOrderAmended*>(payload);
                    order_manager_.on_amend_acknowledged(amend_confirmation);
                    break;
                }
                default: return;
            }
        }

        EventType sample_event_type() noexcept {
            const double l0 = lambda_insert_;
            const double l1 = lambda_cancel_;
            const double l2 = lambda_amend_;

            const double total = l0 + l1 + l2;

            cp_[0] = l0 / total;
            cp_[1] = (l0 + l1) / total;
            cp_[2] = 1.0;
            
            size_t k = rng_->categorical(cp_);

            switch (k) {
                case 0:
                    return EventType::INSERT_ORDER;
                case 1:
                    return EventType::CANCEL_ORDER;
                default:
                    return EventType::AMEND_ORDER;
            }
        }

        void generate_insert() {
            Id_t request_id = request_id_++;
            InsertDecision insert = dynamics_.decide_insert(state_, rng_.get());

            PayloadInsertOrder payload = make_insert_order(
                request_id,
                insert.side,
                insert.price,
                insert.quantity,
                insert.lifespan
            );

            connection_.send_message(
                static_cast<Message_t>(MessageType::INSERT_ORDER),
                &payload,
                SendMode::ASAP
            );
        }

        void generate_cancel() {
            Id_t request_id = request_id_++;
            std::optional<CancelDecision> cancel = dynamics_.decide_cancel(state_, order_manager_, rng_.get());

            if (!cancel) {
                return;
            }

            PayloadCancelOrder payload = make_cancel_order(request_id, cancel->order_id);

            connection_.send_message(
                static_cast<Message_t>(MessageType::CANCEL_ORDER),
                &payload,
                SendMode::ASAP
            );
        }

        void generate_amend() {
            Id_t request_id = request_id_++;
            std::optional<AmendDecision> amend = dynamics_.decide_amend(state_, order_manager_, rng_.get());

            if (!amend) {
                return;
            }

            PayloadAmendOrder payload = make_amend_order(
                request_id,
                amend->order_id,
                amend->new_quantity
            );

            connection_.send_message(
                static_cast<Message_t>(MessageType::AMEND_ORDER),
                &payload,
                SendMode::ASAP
            );
        }

    private:
        boost::asio::io_context& context_;
        std::unique_ptr<RNG> rng_;

        double lambda_insert_{0.0};
        double lambda_cancel_{0.0};
        double lambda_amend_{0.0};
        std::vector<double> cp_;

        std::atomic<bool> running_{false};
        Id_t request_id_;

        ShadowOrderBook shadow_order_book_;
        MarketDynamics<N> dynamics_;
        SimulationState<N> state_;
        OrderManager order_manager_;

        boost::asio::strand<boost::asio::any_io_executor> on_message_strand_;
        Connection connection_;
};
