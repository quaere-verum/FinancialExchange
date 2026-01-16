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

constexpr double LAMBDA_INSERT_BASE = 5'000.0;
constexpr double LAMBDA_CANCEL_BASE = 2'500.0;

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
            , request_id_(0)
            , order_manager_(context, connection_, request_id_) {
                connection_.message_received = [this](IConnection* from, Message_t type, const uint8_t* data) {
                    this->on_message(
                        static_cast<Connection*>(from),
                        type,
                        data
                    );
                };
                connection_.async_read();
                populate_initial_book();
            }

        ~MarketSimulator() {stop();}

        void start() {
            running_ = true;
            PayloadSubscribe sub = make_subscribe(0);
            connection_.send_message(
                static_cast<Message_t>(MessageType::SUBSCRIBE),
                &sub,
                SendMode::ASAP
            );
            schedule_next_event();
        }

        void stop() {running_ = false;}

    private:
        void populate_initial_book() {
            Price_t initial_mid_price = 1'000;
            Price_t initial_spread = 4;

            Price_t best_bid_price = initial_mid_price - initial_spread / 2;
            Price_t best_ask_price = initial_mid_price + initial_spread / 2;

            Volume_t base_qty = 20;

            size_t max_depth = 5;
            for (size_t depth = 0; depth < max_depth; depth++) {

                Id_t buy_request_id = request_id_++;
                connection_.send_message(
                    static_cast<Message_t>(MessageType::INSERT_ORDER),
                    &make_insert_order(
                        buy_request_id,
                        Side::BUY,
                        best_bid_price - depth,
                        base_qty * (max_depth - depth),
                        Lifespan::GOOD_FOR_DAY
                    ),
                    SendMode::ASAP
                );
                order_manager_.register_pending_insert(buy_request_id, 10.0);

                Id_t sell_request_id = request_id_++;
                connection_.send_message(
                    static_cast<Message_t>(MessageType::INSERT_ORDER),
                    &make_insert_order(
                        sell_request_id,
                        Side::SELL,
                        best_ask_price + depth,
                        base_qty * (max_depth - depth),
                        Lifespan::GOOD_FOR_DAY
                    ),
                    SendMode::ASAP
                );
                order_manager_.register_pending_insert(sell_request_id, 10.0);
            }

        }
        
        void schedule_next_event() {
            const double dt = rng_->exponential(lambda_insert_);

            using steady_duration = boost::asio::steady_timer::duration;
            auto timer = std::make_shared<boost::asio::steady_timer>(
                on_message_strand_,
                std::chrono::duration_cast<steady_duration>(
                    std::chrono::duration<double>(dt)
                )
            );

            timer->async_wait(
                [this, timer, dt]
                (const boost::system::error_code& ec) {
                    if (!ec && running_) {
                        state_.sync_with_book(shadow_order_book_, dt);

                        cumulative_hazard_ += lambda_cancel_ * dt;
                        order_manager_.on_hazard_advanced(cumulative_hazard_);

                        dynamics_.update_intensity(
                            state_,
                            order_manager_.open_order_count(),
                            lambda_insert_,
                            lambda_cancel_
                        );

                        generate_insert();
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
                } case MessageType::PARTIAL_FILL_ORDER: {
                    const PayloadPartialFill* partial_fill = reinterpret_cast<const PayloadPartialFill*>(payload);
                    order_manager_.on_partial_fill(partial_fill);
                }
                default: return;
            }
        }

        void generate_insert() {
            Id_t request_id = request_id_++;
            InsertDecision insert = dynamics_.decide_insert(state_, cumulative_hazard_, rng_.get());
            std::cout << "[MarketSimulator] generate_insert() request_id=" << request_id << "\n";
            PayloadInsertOrder payload = make_insert_order(
                request_id,
                insert.side,
                insert.price,
                insert.quantity,
                insert.lifespan
            );
            order_manager_.register_pending_insert(request_id, insert.cancellation_hazard_mass);
            connection_.send_message(
                static_cast<Message_t>(MessageType::INSERT_ORDER),
                &payload,
                SendMode::ASAP
            );
        }


    private:
        boost::asio::io_context& context_;
        std::unique_ptr<RNG> rng_;

        double lambda_insert_{LAMBDA_INSERT_BASE};
        double lambda_cancel_{LAMBDA_CANCEL_BASE};
        double cumulative_hazard_{0.0};

        std::atomic<bool> running_{false};
        std::atomic<Id_t> request_id_;

        ShadowOrderBook shadow_order_book_;
        MarketDynamics<N> dynamics_;
        SimulationState<N> state_;
        OrderManager order_manager_;

        boost::asio::strand<boost::asio::any_io_executor> on_message_strand_;
        Connection connection_;
};
