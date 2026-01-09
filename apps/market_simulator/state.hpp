#pragma once
#include "shadow_order_book.hpp"

struct TimeState {
    double sim_time;
    double time_since_event;
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
    std::array<Price_t, N> bucket_bounds;

    std::array<Volume_t, N> bid_volumes;
    std::array<Volume_t, N> ask_volumes;
    std::array<double, N> imbalances;

    std::array<double, N> bid_mean_distances;
    std::array<double, N> bid_variances;
    std::array<double, N> bid_skews;

    std::array<double, N> ask_mean_distances;
    std::array<double, N> ask_variances;
    std::array<double, N> ask_skews;

    bool has_bid_side;
    bool has_ask_side;
};

struct VolatilityState {
    double realised_variance_short = 0.0;
    double realised_variance_long = 0.0;
    double realised_variance_up = 0.0;
    double realised_variance_down = 0.0;
    double vol_of_vol = 0.0;
    double jump_intensity = 0.0;

    double realised_vol_short() const {return std::sqrt(realised_variance_short);}
    double realised_vol_long() const {return std::sqrt(realised_variance_long);}
    double realised_vol_up() const {return std::sqrt(realised_variance_up);}
    double realised_vol_down() const {return std::sqrt(realised_variance_down);}
};

struct FlowState {
    double abs_volume_ewma = 0.0;
    double trade_rate_ewma = 0.0;
    double buy_volume_ewma = 0.0;
    double sell_volume_ewma = 0.0;
    double volume_surprise = 0.0;
    double signed_volume_ewma = 0.0;
    double flow_imbalance = 0.0;
};

struct WeightedMoments {
    double mean = 0.0;
    double variance = 0.0;
    double skew = 0.0;
};

inline WeightedMoments compute_weighted_moments(
    double w_sum,
    double x_sum,
    double x2_sum,
    double x3_sum
) {
    WeightedMoments m;
    if (w_sum <= 0.0) return m;

    m.mean = x_sum / w_sum;
    m.variance = std::max(0.0, x2_sum / w_sum - m.mean * m.mean);

    if (m.variance > 0.0) {
        double std = std::sqrt(m.variance);
        m.skew =
            (x3_sum / w_sum
             - 3.0 * m.mean * m.variance
             - m.mean * m.mean * m.mean)
            / (std * std * std);
    }
    return m;
}


template<size_t N>
class SimulationState {
    public:
        SimulationState(const std::array<Price_t, N>& liquidity_bucket_bounds) {
            liq_state_.bucket_bounds = liquidity_bucket_bounds;
        }

        void sync_with_book(const ShadowOrderBook& order_book, double dt) {
            update_price_state(order_book);
            update_liq_state(order_book);
            update_time_state(dt);
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

    private:
        void update_time_state(double dt) {
            time_state_.sim_time += dt;
            time_state_.time_since_event = dt;
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

            bid_w_.fill(0);
            bid_x_.fill(0);
            bid_x2_.fill(0);
            bid_x3_.fill(0);

            ask_w_.fill(0);
            ask_x_.fill(0);
            ask_x2_.fill(0);
            ask_x3_.fill(0);


            if (best_bid) {
                for (const auto& [price, volume] : order_book.bids()) {
                    const double dist = static_cast<double>(*best_bid - price);
                    if (dist < 0.0) continue;

                    for (size_t i = 0; i < N; ++i) {
                        if (dist <= liq_state_.bucket_bounds[i]) {
                            liq_state_.bid_volumes[i] += volume;

                            const double w = static_cast<double>(volume);
                            bid_w_[i]  += w;
                            bid_x_[i]  += w * dist;
                            bid_x2_[i] += w * dist * dist;
                            bid_x3_[i] += w * dist * dist * dist;
                        }
                    }
                }
            }

            if (best_ask) {
                for (const auto& [price, volume] : order_book.asks()) {
                    const double dist = static_cast<double>(price - *best_ask);
                    if (dist < 0.0) continue;

                    for (size_t i = 0; i < N; ++i) {
                        if (dist <= liq_state_.bucket_bounds[i]) {
                            liq_state_.ask_volumes[i] += volume;

                            const double w = static_cast<double>(volume);
                            ask_w_[i]  += w;
                            ask_x_[i]  += w * dist;
                            ask_x2_[i] += w * dist * dist;
                            ask_x3_[i] += w * dist * dist * dist;
                        }
                    }
                }
            }
            

            constexpr double eps = 1e-9;

            for (size_t i = 0; i < N; ++i) {
                auto bid_m = compute_weighted_moments(bid_w_[i], bid_x_[i], bid_x2_[i], bid_x3_[i]);
                auto ask_m = compute_weighted_moments(ask_w_[i], ask_x_[i], ask_x2_[i], ask_x3_[i]);

                liq_state_.bid_mean_distances[i] = bid_m.mean;
                liq_state_.bid_variances[i] = bid_m.variance;
                liq_state_.bid_skews[i] = bid_m.skew;

                liq_state_.ask_mean_distances[i] = ask_m.mean;
                liq_state_.ask_variances[i] = ask_m.variance;
                liq_state_.ask_skews[i] = ask_m.skew;

                const double vb = static_cast<double>(liq_state_.bid_volumes[i]);
                const double va = static_cast<double>(liq_state_.ask_volumes[i]);

                liq_state_.imbalances[i] = (vb - va) / (vb + va + eps);
            }
        }
        
        inline void update_vol_state(const PayloadTradeEvent* trade, double dt) {
            const double p0 = static_cast<double>(last_trade_price_);
            const double p1 = static_cast<double>(trade->price);

            const double r  = std::log(p1 / p0);
            const double r2 = r * r;

            VolatilityState& vs = vol_state_;
            const double vol_prev = std::sqrt(vs.realised_variance_short); // Need this for downstream calculation

            const double a_short = 1.0 - std::exp(-dt / TAU_SHORT);
            const double a_long  = 1.0 - std::exp(-dt / TAU_LONG);

            vs.realised_variance_short = (1.0 - a_short) * vs.realised_variance_short + a_short * r2;

            vs.realised_variance_long = (1.0 - a_long) * vs.realised_variance_long + a_long * r2;

            if (r > 0.0) {
                vs.realised_variance_up = (1.0 - a_short) * vs.realised_variance_up + a_short * r2;
                vs.realised_variance_down *= (1.0 - a_short);
            } else if (r < 0.0) {
                vs.realised_variance_down = (1.0 - a_short) * vs.realised_variance_down + a_short * r2;
                vs.realised_variance_up *= (1.0 - a_short);
            } else {
                vs.realised_variance_up   *= (1.0 - a_short);
                vs.realised_variance_down *= (1.0 - a_short);
            }

            const double vol_now  = std::sqrt(vs.realised_variance_short);
            const double dvol = vol_now - vol_prev;
            vs.vol_of_vol = (1.0 - a_short) * vs.vol_of_vol + a_short * (dvol * dvol);

            if (vol_now > VOL_MIN) {
                const double jump_score = std::abs(r) / (vol_now * std::sqrt(dt) + 1e-8);
                const double a_jump = 1.0 - std::exp(-dt / TAU_JUMP);

                if (jump_score > 5.0) {
                    vs.jump_intensity = (1.0 - a_jump) * vs.jump_intensity + a_jump * 1.0;
                } else {
                    vs.jump_intensity *= (1.0 - a_jump);
                }
            }
        }

        // TODO: Update on cancel/amend also
        inline void update_flow_state(const PayloadTradeEvent* trade, double dt) {
            FlowState& fs = flow_state_;

            const double vol = static_cast<double>(trade->quantity);
            const double a_flow = 1.0 - std::exp(-dt / TAU_FLOW);
            const double a_rate = 1.0 - std::exp(-dt / TAU_RATE);
            const double a_surp = 1.0 - std::exp(-dt / TAU_SURPRISE);

            fs.abs_volume_ewma = (1.0 - a_flow) * fs.abs_volume_ewma + a_flow * vol;

            const double inst_rate = 1.0 / dt;

            fs.trade_rate_ewma = (1.0 - a_rate) * fs.trade_rate_ewma + a_rate * inst_rate;

            if (trade->taker_side == Side::BUY) {
                fs.buy_volume_ewma = (1.0 - a_flow) * fs.buy_volume_ewma + a_flow * vol;
                fs.sell_volume_ewma *= (1.0 - a_flow);
            } else {
                fs.sell_volume_ewma = (1.0 - a_flow) * fs.sell_volume_ewma + a_flow * vol;
                fs.buy_volume_ewma *= (1.0 - a_flow);
            }

            const double signed_vol = (trade->taker_side == Side::BUY ? vol : -vol);

            fs.signed_volume_ewma = (1.0 - a_flow) * fs.signed_volume_ewma + a_flow * signed_vol;
            fs.flow_imbalance = std::clamp(fs.signed_volume_ewma / (fs.abs_volume_ewma + 1e-8), -1.0, 1.0);

            const double expected_vol = std::max(fs.abs_volume_ewma, 1e-8);

            const double surprise = (vol - expected_vol) / expected_vol;

            fs.volume_surprise = (1.0 - a_surp) * fs.volume_surprise + a_surp * surprise;
        }


        TimeState time_state_;
        PriceState price_state_;
        LiquidityState<N> liq_state_;
        VolatilityState vol_state_;
        FlowState flow_state_;

        Price_t last_trade_price_ = MAXIMUM_ASK + 1;
        Time_t last_trade_timestamp_ = 0;

        std::array<double, N> bid_w_{}, bid_x_{}, bid_x2_{}, bid_x3_{};
        std::array<double, N> ask_w_{}, ask_x_{}, ask_x2_{}, ask_x3_{};

        // Decay time in seconds
        static constexpr double TAU_SHORT = 1.0;
        static constexpr double TAU_LONG = 30.0;
        static constexpr double TAU_JUMP = 10.0;
        static constexpr double TAU_FLOW = 2.0;
        static constexpr double TAU_RATE = 5.0;
        static constexpr double TAU_SURPRISE = 10.0;

        static constexpr double VOL_MIN = 1e-6;
};
