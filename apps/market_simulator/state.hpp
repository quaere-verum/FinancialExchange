#pragma once
#include "shadow_order_book.hpp"
#include "pcg32.hpp"
#include "parameters.hpp"

enum class LatentModel {GBM, OU};

inline constexpr LatentModel latent_model = LatentModel::GBM;

struct TimeState {
    double sim_time;
    double time_since_previous_sync;
};

struct PriceState {
    std::optional<Price_t> best_bid;
    std::optional<Price_t> best_ask;
    std::optional<Price_t> spread;
    Price_t last_trade_price;
};

template<size_t N>
struct LiquidityState {
    static_assert(N > 1, "LiquidityState<N>: N must be greater than 1, defining buckets [b_1, b_2), ..., [b_{N-1}, b_N)");
    std::array<Price_t, N> bucket_bounds;

    std::array<Volume_t, N - 1> bid_volumes;
    std::array<Volume_t, N - 1> ask_volumes;

    bool has_bid_side;
    bool has_ask_side;
};

struct VolatilityState {
    double realised_variance_micro = 0.0;
    double realised_variance_short = 0.0;
    double realised_variance_long = 0.0;
    double realised_vol_micro = 0.0;
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

struct FeatureVector {
    double sim_time = 0.0;
    double time_since_previous_sync = 0.0;

    bool has_bid_side = false;
    bool has_ask_side = false;

    std::optional<Price_t> best_bid = std::nullopt;
    std::optional<Price_t> best_ask = std::nullopt;
    Price_t last_trade_price = INITIAL_FAIR_VALUE;

    Volume_t bid_at_touch = 0;
    Volume_t ask_at_touch = 0;

    double realised_vol_micro = 0.0;
    double realised_vol_short = 0.0;
    double realised_vol_long = 0.0;
    double jump_intensity = 0.0;

    double abs_volume_ewma = 0.0;
    double trade_rate_ewma_short = 0.0;
    double trade_rate_ewma_long = 0.0;
    double volume_surprise = 0.0;
    double signed_volume_ewma = 0.0;
    double flow_imbalance = 0.0;
    double excitement = 0.0;

    double fair_value = INITIAL_FAIR_VALUE;

    // Derived features
    std::optional<double> mid_price = std::nullopt;
    Price_t spread = SPREAD_IF_ONESIDED;
    double imbalance_at_touch = 0.0;
    double depth_at_touch = 0.0;
    double vol_ratio = 1.0;
    double stress01 = 0.0;
    double vol_ratio_micro = 1.0;
    double stress_micro01 = 0.0;
    double toxicity01 = 0.0;
    double spread01 = 0.0;
    double thin01 = 0.0;
    double mid_fair_gap = 0.0;
    double trade_rate01 = 0.0;
    double surprise_signx01 = 0.0;
    double tight01 = 0.0;
    double jump01 = 0.0;
    double excitement01 = 0.0;
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
            update_features();
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

        const FeatureVector& get_features() const {
            return features_;
        }

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

            const double a_micro = 1.0 - std::exp(-dt / TAU_MICRO);
            const double a_short = 1.0 - std::exp(-dt / TAU_SHORT);
            const double a_long  = 1.0 - std::exp(-dt / TAU_LONG);

            vs.realised_variance_micro = (1.0 - a_micro) * vs.realised_variance_micro + a_micro * r2;

            vs.realised_variance_short = (1.0 - a_short) * vs.realised_variance_short + a_short * r2;

            vs.realised_variance_long = (1.0 - a_long) * vs.realised_variance_long + a_long * r2;

            vs.realised_vol_micro = std::sqrt(vs.realised_variance_micro);
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

        inline void update_features() {
            features_.sim_time = time_state_.sim_time;
            features_.time_since_previous_sync = time_state_.time_since_previous_sync;
            features_.has_ask_side = liq_state_.has_ask_side;
            features_.has_bid_side = liq_state_.has_bid_side;
            features_.best_ask = price_state_.best_ask;
            features_.best_bid = price_state_.best_bid;
            features_.last_trade_price = price_state_.last_trade_price;
            features_.ask_at_touch = liq_state_.ask_volumes[0];
            features_.bid_at_touch = liq_state_.bid_volumes[0];
            features_.realised_vol_micro = vol_state_.realised_vol_micro;
            features_.realised_vol_short = vol_state_.realised_vol_short;
            features_.realised_vol_long = vol_state_.realised_vol_long;
            features_.jump_intensity = vol_state_.jump_intensity;
            features_.abs_volume_ewma = flow_state_.abs_volume_ewma;
            features_.trade_rate_ewma_short = flow_state_.trade_rate_ewma_short;
            features_.trade_rate_ewma_long = flow_state_.trade_rate_ewma_long;
            features_.volume_surprise = flow_state_.volume_surprise;
            features_.signed_volume_ewma = flow_state_.signed_volume_ewma;
            features_.flow_imbalance = flow_state_.flow_imbalance;
            features_.excitement = flow_state_.excitement;
            features_.fair_value = latent_state_.fair_value;

            // Derived features
            features_.mid_price = 
                (features_.has_ask_side && features_.has_bid_side) ? 
                ((double)*features_.best_ask + (double)*features_.best_bid) / 2.0 : (double)features_.last_trade_price;
            features_.spread = 
                (features_.has_ask_side && features_.has_bid_side) ? 
                (*features_.best_ask - *features_.best_bid): SPREAD_IF_ONESIDED;
            features_.depth_at_touch = features_.ask_at_touch + features_.bid_at_touch;
            features_.imbalance_at_touch = (features_.bid_at_touch - features_.ask_at_touch) / (features_.depth_at_touch + EPS);
            features_.vol_ratio = features_.realised_vol_short / (features_.realised_vol_long + EPS);
            features_.vol_ratio_micro = features_.realised_vol_micro / (features_.realised_vol_short + EPS);
            features_.spread01 = std::clamp((double)features_.spread, 0.0, SPREAD_CLIP_RANGE) / SPREAD_CLIP_RANGE;
            features_.mid_fair_gap = features_.mid_price ? features_.fair_value - (double)*features_.mid_price : SPREAD_IF_ONESIDED;
            {
                const double depth = features_.depth_at_touch;
                const double flow  = std::max((double)features_.abs_volume_ewma, EPS);
                const double depth_time = depth / flow; // "seconds-to-clear-touch"
                features_.thin01 = clamp01(std::exp(-depth_time / DEPTH_TIME_SCALE));
            }
            {
                // Volume surprise is between -1.0 and infinity (EWMA of incoming volume vs. expected volume)
                const double surprise_raw = (double)features_.volume_surprise;
                const double sign = surprise_raw < 0.0 ? -1.0 : 1.0;
                const double surprise_log = std::log1p(std::fabs(surprise_raw) + EPS);
                features_.surprise_signx01 = sign * (1.0 - std::exp(-surprise_log / SURPRISE_LOG_SCALE));
            }
            {
                const double s = std::max((double)features_.trade_rate_ewma_short, EPS);
                const double l = std::max((double)features_.trade_rate_ewma_long,  EPS);
                const double abs_log_r = std::fabs(std::log(s / l));
                features_.trade_rate01 = 1.0 - std::exp(-abs_log_r * TRADE_RATE_SCALE);
            }
            {
                if (features_.has_ask_side && features_.has_bid_side) {
                    const double spread_clipped = clamp01((features_.spread) / SPREAD_CLIP_RANGE);
                    const double tight_spread01 = 1.0 - spread_clipped;
                    const double not_thin01 = 1.0 - features_.thin01;
                    features_.tight01 = clamp01(tight_spread01 * (0.7 + 0.3 * not_thin01));
                } else {
                    features_.tight01 = 0.0;
                }
            }
            {
                const double jump_intensity = std::max((double)features_.jump_intensity, 0.0);
                features_.jump01 = clamp01(1.0 - std::exp(-jump_intensity / JUMP_INTENSITY_SCALE));
            }
            {
                const double abs_vol = std::max((double)features_.abs_volume_ewma, EPS);
                const double dir = std::fabs((double)features_.signed_volume_ewma) / abs_vol;
                const double dir01 = sat01(dir);

                double gap01 = 0.0;
                if (features_.mid_price) {
                    const double gap = std::fabs(features_.fair_value - (double)*features_.mid_price);
                    const double gap_in_spread = gap / (double)features_.spread;
                    gap01 = clamp01(std::tanh(gap_in_spread / GAP_IN_SPREAD_SCALE));
                } else {
                    gap01 = 0.0; // no mid/spread => don't hallucinate toxicity from it
                }

                const double s01 = features_.surprise_signx01;
                features_.toxicity01 = clamp01(
                      W_DIR_TO_TOX  * dir01 
                    + W_GAP_TO_TOX  * gap01 
                    + W_SURP_TO_TOX * s01
                );
            }
            {
                features_.excitement01 = features_.excitement / EXCITE_CAP;
            }
            {
                const double spread_term = std::pow(features_.spread01, TERM_GAMMA);

                const double vol_ratio = std::max((double)features_.vol_ratio, EPS);
                const double vol_term = std::pow(tanh01(std::log(vol_ratio) / 0.7), TERM_GAMMA);

                const double thin_term = std::pow(features_.thin01, TERM_GAMMA);
                const double exc_term = std::pow(features_.excitement01, TERM_GAMMA);

                features_.stress01 = clamp01(
                    W_SPREAD_TO_STRESS * spread_term +
                    W_VOL_TO_STRESS    * vol_term +
                    W_THIN_TO_STRESS   * thin_term +
                    W_EXC_TO_STRESS    * exc_term
                );
            }
            {
                const double thin_m  = std::pow(features_.thin01, MICRO_GAMMA);
                const double vol_m   = std::pow(tanh01(std::log(features_.vol_ratio_micro + EPS) / 0.5), MICRO_GAMMA);
                const double exc_m   = std::pow(features_.excitement01, MICRO_GAMMA);
                const double tox_m   = std::pow(features_.toxicity01, MICRO_GAMMA);

                // Optional: only a small spread contribution
                const double spr_m   = std::pow(features_.spread01, MICRO_GAMMA);

                double raw = 
                      W_THIN_TO_STRESS_MICRO * thin_m 
                    + W_VOL_TO_STRESS_MICRO * vol_m 
                    + W_EXC_TO_STRESS_MICRO * exc_m 
                    + W_TOX_TO_STRESS_MICRO * tox_m 
                    + W_SPREAD_TO_STRESS_MICRO * spr_m;
                features_.stress_micro01 = 1.0 - std::exp(-raw * 2.0);
            }
        }

        static inline double clamp01(double x) {
            return std::min(1.0, std::max(0.0, x));
        }

        static inline double sat01(double x) {
            return x / (1.0 + x);
        }

        static inline double tanh01(double x) {
            return 0.5 * (std::tanh(x) + 1.0);
        }


        TimeState time_state_;
        PriceState price_state_;
        LiquidityState<N> liq_state_;
        VolatilityState vol_state_;
        FlowState flow_state_;
        LatentState latent_state_;
        FeatureVector features_;

        // Update later to be more general
        PCGRNG rng_;

        Price_t last_trade_price_ = static_cast<Price_t>(INITIAL_FAIR_VALUE);
        Time_t last_trade_timestamp_ = 0;

        // Decay time in seconds
        static constexpr double TAU_MICRO = 0.05;
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
