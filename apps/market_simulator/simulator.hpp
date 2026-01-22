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

constexpr size_t MESSAGES_PER_DRAIN = 2'000;

template <size_t N>
class MarketSimulator {
    public:
        MarketSimulator(
            boost::asio::io_context& context,
            tcp::socket socket,
            std::unique_ptr<RNG> rng,
            const std::array<Price_t, N>& liquidity_bucket_bounds,
            std::function<void(Connection*)> on_shutdown
        )
        : context_(context)
        , sim_strand_(boost::asio::make_strand(context))
        , event_timer_(context)
        , rng_(std::move(rng))
        , connection_(context, std::move(socket), 0, inbound_, outbound_)
        , state_(liquidity_bucket_bounds)
        , request_id_(0)
        , order_manager_(sim_strand_, connection_, request_id_) {
            connection_.large_message_received = [this](Id_t cid, Message_t type, std::shared_ptr<std::vector<uint8_t>> buf) {
                this->on_large_message(cid, type, buf);
            };
            connection_.disconnected = [this, on_shutdown = std::move(on_shutdown)](Connection* c) {
                running_.store(false, std::memory_order_release);
                event_timer_.cancel();
                if (on_shutdown) on_shutdown(c);
            };

            connection_.inbound_ready = [this] {
                if (!running_.load(std::memory_order_acquire)) return;
                schedule_inbound_drain_();
            };

        }


        ~MarketSimulator() {stop();}

        void start() {
            running_.store(true, std::memory_order_release);
            connection_.async_read();
            populate_initial_book();
            boost::asio::post(sim_strand_, [this]{ schedule_next_event(); });
        }

        void stop() {
            running_.store(false, std::memory_order_release);
            boost::asio::dispatch(sim_strand_, [this] {
                event_timer_.cancel();
                connection_.close();
            });
        }


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
                    )
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
                    )
                );
                order_manager_.register_pending_insert(sell_request_id, 10.0);
            }

        }
        
        void schedule_next_event() {
            const double dt = rng_->exponential(lambda_insert_);

            event_timer_.expires_after(
                std::chrono::duration_cast<boost::asio::steady_timer::duration>(
                    std::chrono::duration<double>(dt)
                )
            );

            event_timer_.async_wait(
                boost::asio::bind_executor(
                    sim_strand_,
                    [this, dt](const boost::system::error_code& ec) {
                        if (ec || !running_.load(std::memory_order_acquire)) return;

                        state_.sync_with_book(shadow_order_book_, dt);
                        order_manager_.update_cancel_rate(lambda_cancel_);
                        dynamics_.update_intensity(
                            state_,
                            order_manager_.open_order_count(),
                            lambda_insert_,
                            lambda_cancel_
                        );

                        generate_insert();
                        schedule_next_event();
                    }
                )
            );
        }

        void drain_inbound_bounded(size_t max_msgs) {
            InboundMessage msg{};
            for (size_t i = 0; i < max_msgs && inbound_.try_pop(msg); ++i) {
                on_message(msg.message_type, msg.payload.data());
            }
        }

        void schedule_inbound_drain_() {
            bool expected = false;
            if (!inbound_drain_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                return;
            }

            boost::asio::post(sim_strand_, [this] {
                inbound_drain_scheduled_.store(false, std::memory_order_release);

                drain_inbound_bounded(MESSAGES_PER_DRAIN);

                if (running_.load(std::memory_order_acquire) && inbound_.size_approx() != 0) {
                    schedule_inbound_drain_();
                }
            });
        }


        void on_message(Message_t message_type, const uint8_t* payload) {
            switch (static_cast<MessageType>(message_type)) {
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
                    break;
                }
                default: return;
            }
        }

        void on_large_message(Id_t /*connection_id*/, Message_t message_type, const std::shared_ptr<std::vector<uint8_t>>& buf) {
            if (!buf) return;

            switch (static_cast<MessageType>(message_type)) {
                case MessageType::ORDER_BOOK_SNAPSHOT: {
                    if (buf->size() != sizeof(PayloadOrderBookSnapshot)) return;

                    const auto* snap = reinterpret_cast<const PayloadOrderBookSnapshot*>(buf->data());

                    shadow_order_book_.on_order_book_snapshot(snap);
                    return;
                }
                default:
                    return;
            }
        }


        void generate_insert() {
            Id_t request_id = request_id_++;
            InsertDecision insert = dynamics_.decide_insert(state_, order_manager_.cumulative_hazard(), rng_.get());
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
                &payload
            );
        }


    private:
        boost::asio::io_context& context_;
        boost::asio::strand<boost::asio::io_context::executor_type> sim_strand_;
        boost::asio::steady_timer event_timer_;
        std::unique_ptr<RNG> rng_;
        InboundQueue inbound_;
        OutboundQueue outbound_;

        Connection connection_;

        double lambda_insert_{LAMBDA_INSERT_BASE};
        double lambda_cancel_{LAMBDA_CANCEL_BASE};

        std::atomic<bool> running_{false};
        std::atomic<bool> inbound_drain_scheduled_{false};
        std::atomic<Id_t> request_id_{0};

        ShadowOrderBook shadow_order_book_;
        MarketDynamics<N> dynamics_;
        SimulationState<N> state_;
        OrderManager order_manager_;
};
