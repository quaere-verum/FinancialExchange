#pragma once
#include "shadow_order_book.hpp"
#include "pcg32.hpp"

enum class LatentModel {GBM, OU};

inline constexpr LatentModel latent_model = LatentModel::GBM;
constexpr double INITIAL_FAIR_VALUE = 1'000.0;
constexpr double INITIAL_LOG_FAIR_VALUE = 6.907755278982137; // log(1000)

struct TimeState {
    double sim_time;
    double time_since_previous_sync;
};

struct PriceState {
    std::optional<Price_t> best_bid;
    std::optional<Price_t> best_ask;
    std::optional<Price_t> spread;
    Price_t last_trade_price;

    const std::optional<Price_t> mid_price() const {
        if (best_bid && best_ask) {
            return (*best_bid + *best_ask) / 2;
        }
        return std::nullopt;
    }
};

template<size_t N>
struct LiquidityState {
    static_assert(N > 1, "LiquidityState<N>: N must be greater than 1, defining buckets [b_1, b_2), ..., [b_{N-1}, b_N)");
    std::array<Price_t, N> bucket_bounds;

    std::array<Volume_t, N - 1> bid_volumes;
    std::array<Volume_t, N - 1> ask_volumes;
    std::array<double, N - 1> imbalances;

    bool has_bid_side;
    bool has_ask_side;
};

struct VolatilityState {
    double realised_variance_short = 0.0;
    double realised_variance_long = 0.0;
    double realised_vol_short = 0.0;
    double realised_vol_long = 0.0;
    double jump_intensity = 0.0;
};

struct FlowState {
    double abs_volume_ewma = 0.0;
    double trade_rate_ewma_short = 0.0;
    double trade_rate_ewma_long = 0.0;
    double volume_surprise = 0.0;
    double signed_volume_ewma = 0.0;
    double flow_imbalance = 0.0;
    double excitement = 0.0;
};

struct LatentState {
    double fair_value = INITIAL_FAIR_VALUE;
    double log_fair_value = INITIAL_LOG_FAIR_VALUE;
    double time_since_update = 0.0;
};

template<size_t N>
class SimulationState {
    public:
        SimulationState(const std::array<Price_t, N>& liquidity_bucket_bounds)
        : rng_(0, 0) {
            liq_state_.bucket_bounds = liquidity_bucket_bounds;
        }

        void sync_with_book(const ShadowOrderBook& order_book, double dt) {
            update_price_state(order_book);
            update_liq_state(order_book);
            update_time_state(dt);
            update_latent_state(dt);
        }

        void on_trade(const PayloadTradeEvent* trade) {
            if (last_trade_timestamp_ == 0) {
                last_trade_price_ = trade->price;
                last_trade_timestamp_ = trade->timestamp;
                return;
            }
            const double dt = std::max(1e-6, (trade->timestamp - last_trade_timestamp_) * 1e-9); // ns -> seconds
            if (dt <= 0.0) {return;}
            update_vol_state(trade, dt);
            update_flow_state(trade, dt);
            last_trade_price_ = trade->price;
            last_trade_timestamp_ = trade->timestamp;
        }

        const TimeState time_state() const {return time_state_;}
        const PriceState price_state() const {return price_state_;}
        const LiquidityState<N> liq_state() const {return liq_state_;}
        const VolatilityState vol_state() const {return vol_state_;}
        const FlowState flow_state() const {return flow_state_;}
        const LatentState latent_state() const {return latent_state_;}

    private:
        void update_time_state(double dt) {
            time_state_.sim_time += dt;
            time_state_.time_since_previous_sync = dt;
        }

        inline void update_price_state(const ShadowOrderBook& order_book) {
            price_state_.best_bid = order_book.best_bid_price();
            price_state_.best_ask = order_book.best_ask_price();
            price_state_.last_trade_price = last_trade_price_;
            if (price_state_.best_bid && price_state_.best_ask) {
                price_state_.spread = *price_state_.best_ask - *price_state_.best_bid;
            } else {
                price_state_.spread = std::nullopt;
            }
        }

        inline void update_liq_state(const ShadowOrderBook& order_book) {
            const auto best_bid = order_book.best_bid_price();
            const auto best_ask = order_book.best_ask_price();

            liq_state_.has_bid_side = best_bid.has_value();
            liq_state_.has_ask_side = best_ask.has_value();

            liq_state_.bid_volumes.fill(0);
            liq_state_.ask_volumes.fill(0);

            if (best_bid) {
                for (const auto& [price, volume] : order_book.bids()) {
                    const double dist = static_cast<double>(*best_bid - price);
                    if (dist < 0.0) continue;

                    for (size_t i = 0; i < N - 1; ++i) {
                        if (liq_state_.bucket_bounds[i] <= dist && dist < liq_state_.bucket_bounds[i + 1]) {
                            liq_state_.bid_volumes[i] += volume;
                        }
                    }
                }
            }

            if (best_ask) {
                for (const auto& [price, volume] : order_book.asks()) {
                    const double dist = static_cast<double>(price - *best_ask);
                    if (dist < 0.0) continue;

                    for (size_t i = 0; i < N - 1; ++i) {
                        if (liq_state_.bucket_bounds[i] <= dist && dist < liq_state_.bucket_bounds[i + 1]) {
                            liq_state_.ask_volumes[i] += volume;
                        }
                    }
                }
            }

            for (size_t i = 0; i < N - 1; ++i) {
                double vb = (double)liq_state_.bid_volumes[i];
                double va = (double)liq_state_.ask_volumes[i];
                liq_state_.imbalances[i] = (vb - va) / (vb + va + 1e-9);
            }
        }
        
        inline void update_latent_state(double dt) {
            constexpr double sigma  = 0.01;
            constexpr double latent_update_interval = 0.01;

            latent_state_.time_since_update += dt;

            if (latent_state_.time_since_update >= latent_update_interval) {
                const double latent_dt = latent_state_.time_since_update;
                const double z = rng_.standard_normal();

                if constexpr (latent_model == LatentModel::OU) {
                    constexpr double kappa  = 0.05;
                    constexpr double mu_log = INITIAL_LOG_FAIR_VALUE; // log(1000)

                    latent_state_.log_fair_value +=
                        kappa * (mu_log - latent_state_.log_fair_value) * latent_dt
                        + sigma * std::sqrt(latent_dt) * z;

                } else {
                    constexpr double mu = 0.0;
                    latent_state_.log_fair_value +=
                        (mu - 0.5 * sigma * sigma) * latent_dt
                        + sigma * std::sqrt(latent_dt) * z;
                }

                latent_state_.fair_value = std::exp(latent_state_.log_fair_value);
                latent_state_.time_since_update = 0.0;
            }
        }


        inline void update_vol_state(const PayloadTradeEvent* trade, double dt) {
            VolatilityState& vs = vol_state_;
            const double p0 = static_cast<double>(last_trade_price_);
            const double p1 = static_cast<double>(trade->price);

            const double r  = std::log(p1 / p0);
            const double r2 = r * r;

            const double a_short = 1.0 - std::exp(-dt / TAU_SHORT);
            const double a_long  = 1.0 - std::exp(-dt / TAU_LONG);

            vs.realised_variance_short = (1.0 - a_short) * vs.realised_variance_short + a_short * r2;

            vs.realised_variance_long = (1.0 - a_long) * vs.realised_variance_long + a_long * r2;

            vs.realised_vol_short = std::sqrt(vs.realised_variance_short);
            vs.realised_vol_long = std::sqrt(vs.realised_variance_long);
            if (vs.realised_vol_short > VOL_MIN) {
                const double jump_score = std::abs(r) / (vs.realised_vol_short * std::sqrt(dt) + 1e-8);
                const double a_jump = 1.0 - std::exp(-dt / TAU_JUMP);

                if (jump_score > 5.0) {
                    vs.jump_intensity = (1.0 - a_jump) * vs.jump_intensity + a_jump * 1.0;
                } else {
                    vs.jump_intensity *= (1.0 - a_jump);
                }
            }
        }

        inline void update_flow_state(const PayloadTradeEvent* trade, double dt) {
            FlowState& fs = flow_state_;

            const double vol = static_cast<double>(trade->quantity);
            const double a_flow  = 1.0 - std::exp(-dt / TAU_FLOW);
            const double a_rate_short  = 1.0 - std::exp(-dt / TAU_RATE_SHORT);
            const double a_rate_long = 1.0 - std::exp(-dt / TAU_RATE_LONG);
            const double a_surp  = 1.0 - std::exp(-dt / TAU_SURPRISE);

            fs.abs_volume_ewma = (1.0 - a_flow) * fs.abs_volume_ewma + a_flow * vol;

            const double inst_rate = 1.0 / dt;

            fs.trade_rate_ewma_short = (1.0 - a_rate_short) * fs.trade_rate_ewma_short + a_rate_short * inst_rate;
            fs.trade_rate_ewma_long = (1.0 - a_rate_long) * fs.trade_rate_ewma_long + a_rate_long * inst_rate;

            const double signed_vol = (trade->taker_side == Side::BUY ? vol : -vol);

            fs.signed_volume_ewma = (1.0 - a_flow) * fs.signed_volume_ewma + a_flow * signed_vol;
            fs.flow_imbalance = std::clamp(fs.signed_volume_ewma / (fs.abs_volume_ewma + 1e-8), -1.0, 1.0);

            const double expected_vol = std::max(fs.abs_volume_ewma, 1e-8);
            const double surprise = (vol - expected_vol) / expected_vol;
            fs.volume_surprise = (1.0 - a_surp) * fs.volume_surprise + a_surp * surprise;

            const double excitement_decay = std::exp(-dt / TAU_EXCITE);
            fs.excitement *= excitement_decay;
            
            const double q = (double)trade->quantity;
            const double q_scale = std::max(1e-6, fs.abs_volume_ewma);

            // Dimensionless marked impulse; heavy-tail tolerant
            const double g_size = std::log1p(q / q_scale);
            const double g_surp = std::max(0.0, fs.volume_surprise);
            const double g = std::clamp(0.8 * g_size + 0.2 * g_surp, 0.0, 6.0);

            fs.excitement += EXCITE_ALPHA * g;
            fs.excitement = std::min(fs.excitement, EXCITE_CAP);
        }


        TimeState time_state_;
        PriceState price_state_;
        LiquidityState<N> liq_state_;
        VolatilityState vol_state_;
        FlowState flow_state_;
        LatentState latent_state_;

        // Update later to be more general
        PCGRNG rng_;

        Price_t last_trade_price_ = static_cast<Price_t>(INITIAL_FAIR_VALUE);
        Time_t last_trade_timestamp_ = 0;

        // Decay time in seconds
        static constexpr double TAU_SHORT = 1.0;
        static constexpr double TAU_LONG = 30.0;
        static constexpr double TAU_JUMP = 10.0;
        static constexpr double TAU_FLOW = 2.0;
        static constexpr double TAU_RATE_SHORT = 3.0;
        static constexpr double TAU_RATE_LONG = 30.0;
        static constexpr double TAU_SURPRISE = 10.0;
        static constexpr double TAU_EXCITE = 0.15;

        static constexpr double VOL_MIN = 1e-6;
        static constexpr double EXCITE_ALPHA = 0.06;
        static constexpr double EXCITE_CAP = 5.0;
};
