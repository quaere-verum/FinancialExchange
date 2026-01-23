#pragma once

#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <chrono>
#include <mutex>

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
        // , metrics_timer_(context)
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
            // start_metrics_();
            connection_.async_read();
            boost::asio::post(sim_strand_, [this]{
                last_tick_ = std::chrono::steady_clock::now();
                schedule_tick(); 
            });
        }

        void stop() {
            running_.store(false, std::memory_order_release);
            boost::asio::dispatch(sim_strand_, [this] {
                event_timer_.cancel();
                connection_.close();
            });
        }


    private:
        // void start_metrics_() {
        //     metrics_timer_.expires_after(std::chrono::seconds(1));
        //     metrics_timer_.async_wait(boost::asio::bind_executor(
        //         sim_strand_,
        //         [this](const boost::system::error_code& ec) {
        //             if (ec || !running_.load(std::memory_order_acquire)) return;

        //             const double avg_us =
        //                 (tick_count_ == 0) ? 0.0 : (handler_ns_accum_ / 1000.0) / static_cast<double>(tick_count_);

        //             static std::mutex cout_mtx;
        //             std::lock_guard<std::mutex> lk(cout_mtx);
        //             std::cout
        //                 << "[SIM] ticks/s=" << tick_count_
        //                 << " avg_handler_us=" << avg_us
        //                 << " inserts_sent/s=" << inserts_sent_
        //                 << " inbound_processed/s=" << inbound_msgs_processed_
        //                 << " inbound_backlog_max=" << inbound_backlog_max_
        //                 << "\n";

        //             // reset per-second counters
        //             tick_count_ = 0;
        //             handler_ns_accum_ = 0;
        //             inserts_sent_ = 0;
        //             inbound_msgs_processed_ = 0;
        //             inbound_backlog_max_ = 0;

        //             start_metrics_();
        //         }
        //     ));
        // }

        void schedule_tick() {
            event_timer_.expires_after(std::chrono::duration_cast<boost::asio::steady_timer::duration>(tick_));
            event_timer_.async_wait(boost::asio::bind_executor(
                sim_strand_,
                [this](const boost::system::error_code& ec) {
                    if (ec || !running_.load(std::memory_order_acquire)) return;

                    if (inbound_.size_approx() != 0) {
                        drain_inbound_bounded(MESSAGES_PER_DRAIN);
                    }
                    
                    const auto t0 = std::chrono::steady_clock::now();
                    double dt = std::chrono::duration<double>(t0 - last_tick_).count();
                    if (dt < 0.0) dt = 0.0;
                    if (dt > 0.25) dt = 0.25;
                    last_tick_ = t0;
                    state_.sync_with_book(shadow_order_book_, dt);
                    order_manager_.update_cancel_rate(lambda_cancel_);
                    dynamics_.update_intensity(state_, order_manager_.open_order_count(), lambda_insert_, lambda_cancel_);

                    const double mean = lambda_insert_ * dt;
                    const std::uint32_t k = rng_->poisson(mean);

                    for (std::uint32_t i = 0; i < k; ++i) {
                        generate_insert();
                    }

                    // const auto t1 = std::chrono::steady_clock::now();
                    // handler_ns_accum_ += (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    // tick_count_ += 1;

                    schedule_tick();
                }
            ));
        }


        void drain_inbound_bounded(size_t max_msgs) {
            InboundMessage msg{};
            std::size_t n = 0;
            for (size_t i = 0; i < max_msgs && inbound_.try_pop(msg); ++i) {
                on_message(msg.message_type, msg.payload.data());
                ++n;
            }
            // inbound_msgs_processed_ += n;

            // const auto backlog = inbound_.size_approx();
            // if (backlog > inbound_backlog_max_) inbound_backlog_max_ = backlog;
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
            // inserts_sent_ += 1;
        }


    private:
        boost::asio::io_context& context_;
        boost::asio::strand<boost::asio::io_context::executor_type> sim_strand_;
        boost::asio::steady_timer event_timer_;
        std::chrono::duration<double> tick_{0.001};
        std::chrono::steady_clock::time_point last_tick_{};

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

        // // metrics
        // boost::asio::steady_timer metrics_timer_;
        // std::uint64_t tick_count_{0};
        // std::uint64_t handler_ns_accum_{0};
        // std::uint64_t inserts_sent_{0};
        // std::uint64_t inbound_msgs_processed_{0};
        // std::uint64_t inbound_backlog_max_{0};
};
