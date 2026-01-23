#pragma once
#include "state.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

constexpr double LAMBDA_INSERT_BASE = 25'000.0;
constexpr double CANCEL_SCALING_FACTOR = 50'000.0;
constexpr double LAMBDA_CANCEL_BASE = LAMBDA_INSERT_BASE / CANCEL_SCALING_FACTOR;

constexpr double BASE_ORDER_SIZE = 25.0;

enum class AgentType : uint8_t {
    MM = 0,       // market-maker / near touch provider
    TAKER = 1,    // liquidity taker / urgent
    DEEP = 2,     // deep liquidity provider
    NOISE = 3     // uninformed/noise
};

enum class OrderRegime : uint8_t {
    MARKETABLE = 0,
    IMPROVE = 1,
    PASSIVE = 2
};

struct InsertDecision {
    Side side;
    Price_t price;
    Volume_t quantity;
    Lifespan lifespan;
    double cancellation_hazard_mass; // cancellation threshold mass (larger => longer survival under same hazard accumulation)
};

template<size_t N>
class MarketDynamics {
public:
    InsertDecision decide_insert(const SimulationState<N>& state, double cumulative_hazard, RNG* rng) const {
        const auto& ts  = state.time_state();
        const auto& ps  = state.price_state();
        const auto& vs  = state.vol_state();
        const auto& fs  = state.flow_state();
        const auto& liq = state.liq_state();
        const auto& ls  = state.latent_state();

        // ------------------------------------------------------------
        // 0) Preconditions: if one-sided book, behave more “taker-like”
        // ------------------------------------------------------------
        const bool has_bid = ps.best_bid.has_value();
        const bool has_ask = ps.best_ask.has_value();
        const double spread_ticks = (ps.spread ? static_cast<double>(*ps.spread) : 0.0);

        // ------------------------------------------------------------
        // 1) Order sign with persistence (long memory proxy)
        // ------------------------------------------------------------
        double side_score = 0.0;
        side_score += 0.9 * fs.flow_imbalance;            // immediate pressure
        side_score += 0.6 * fs.taker_sign_ewma;           // persistence
        if (liq.has_bid_side && liq.has_ask_side) {
            side_score += 0.4 * liq.imbalances[0];        // near-touch imbalance
        }

        double buy_prob = 0.5 + 0.35 * std::tanh(side_score);
        buy_prob = std::clamp(buy_prob, 0.02, 0.98);
        Side side = rng->bernoulli(buy_prob) ? Side::BUY : Side::SELL;

        // ------------------------------------------------------------
        // 2) Archetype mixture (heterogeneity)
        // ------------------------------------------------------------
        const double vol_s = vs.realised_vol_short();
        const double urgency =
            std::clamp(0.35 * std::abs(fs.flow_imbalance)
                     + 0.35 * std::min(vol_s, 1.0)
                     + 0.30 * std::clamp(fs.trade_excitation / 2.0, 0.0, 1.0),
                     0.0, 1.0);

        // When the book is thin/one-sided, push probability towards TAKER/MM.
        const double thinness =
            (liq.has_bid_side && liq.has_ask_side)
                ? 1.0 / std::sqrt(1.0 + static_cast<double>(liq.bid_volumes[0] + liq.ask_volumes[0]))
                : 1.0;

        // Base weights
        double w_mm    = 0.45 + 0.25 * (1.0 - urgency);
        double w_taker = 0.20 + 0.55 * urgency + 0.20 * thinness;
        double w_deep  = 0.25 + 0.15 * (1.0 - urgency);
        double w_noise = 0.10;

        // Normalize
        double w_sum = w_mm + w_taker + w_deep + w_noise;
        w_mm /= w_sum; w_taker /= w_sum; w_deep /= w_sum; w_noise /= w_sum;

        AgentType agent = sample_agent_(rng, w_mm, w_taker, w_deep, w_noise);

        // ------------------------------------------------------------
        // 3) Choose regime explicitly: MARKETABLE / IMPROVE / PASSIVE
        // ------------------------------------------------------------
        OrderRegime regime = OrderRegime::PASSIVE;
        double p_marketable = 0.0;
        if (agent == AgentType::TAKER) {
            p_marketable = 0.35 + 0.55 * urgency;
        } else if (agent == AgentType::MM) {
            p_marketable = 0.05 + 0.10 * urgency; // MM sometimes sweeps to manage inventory
        } else {
            p_marketable = 0.02 + 0.05 * urgency;
        }

        // Spread impacts market orders (narrow spread encourages crossing)
        if (ps.spread) {
            p_marketable += 0.12 / (1.0 + spread_ticks);
        }
        // One-sided -> more taker-ish to re-form book
        if (!has_bid || !has_ask) {
            p_marketable = std::max(p_marketable, 0.35);
        }
        p_marketable = std::clamp(p_marketable, 0.01, 0.95);

        // Improve-in-spread regime (only if spread > 1 tick)
        double p_improve = 0.0;
        if (ps.spread && *ps.spread > 1) {
            if (agent == AgentType::MM) {
                p_improve = 0.45;
            } else if (agent == AgentType::NOISE) {
                p_improve = 0.10;
            } else {
                p_improve = 0.20;
            }
            // Higher urgency reduces willingness to improve (more crossing)
            p_improve *= (1.0 - 0.6 * urgency);
        }

        // Draw regime
        const double u_reg = rng->standard_uniform();
        if (u_reg < p_marketable) {
            regime = OrderRegime::MARKETABLE;
        } else if (u_reg < p_marketable + p_improve) {
            regime = OrderRegime::IMPROVE;
        } else {
            regime = OrderRegime::PASSIVE;
        }

        // ------------------------------------------------------------
        // 4) Price formation (anchor + regime-specific placement)
        // ------------------------------------------------------------
        const Price_t anchor = compute_anchor_(ps, ls, side, rng);

        Price_t price = anchor;
        double dist = 0.0;

        if (regime == OrderRegime::MARKETABLE) {
            // True marketable price: cross to the opposite best if available.
            if (side == Side::BUY && ps.best_ask) {
                price = *ps.best_ask;
                dist = has_bid ? std::max(0.0, static_cast<double>(price - *ps.best_bid)) : 0.0;
            } else if (side == Side::SELL && ps.best_bid) {
                price = *ps.best_bid;
                dist = has_ask ? std::max(0.0, static_cast<double>(*ps.best_ask - price)) : 0.0;
            } else {
                // fallback if one-sided: price “through” anchor to force execution if possible
                price = side == Side::BUY ? std::min<Price_t>(MAXIMUM_ASK, anchor + 4) : std::max<Price_t>(MINIMUM_BID, anchor - 4);
                dist = 0.0;
            }
        } else if (regime == OrderRegime::IMPROVE) {
            // Place inside spread by 1 tick
            if (ps.best_bid && ps.best_ask) {
                if (side == Side::BUY) {
                    price = std::min<Price_t>(*ps.best_ask - 1, *ps.best_bid + 1);
                } else {
                    price = std::max<Price_t>(*ps.best_bid + 1, *ps.best_ask - 1);
                }
                dist = 1.0;
            } else {
                // fallback to passive if no two-sided book
                regime = OrderRegime::PASSIVE;
            }
        }

        if (regime == OrderRegime::PASSIVE) {
            // Heavy-tailed placement distance with smooth vol/spread dependence.
            const double vol_l = vs.realised_vol_long();
            const double vol_regime = (vol_l > 0.0) ? (vol_s / vol_l) : 1.0;

            const double spread_scale = (ps.spread ? spread_ticks : 1.0);
            const double base =
                1.0
                + 0.35 * spread_scale
                + 0.25 * std::log1p(5.0 * std::max(0.0, vol_regime - 1.0))
                + 0.35 * vs.jump_intensity;

            // Archetype-dependent mean distance in ticks
            double mean_dist = base;
            if (agent == AgentType::MM)      mean_dist *= 0.8;
            else if (agent == AgentType::DEEP) mean_dist *= 2.5;
            else if (agent == AgentType::NOISE) mean_dist *= 1.3;

            mean_dist = std::clamp(mean_dist, 0.5, 50.0);
            const double d = rng->exponential(mean_dist);
            dist = std::max(0.0, d);

            if (side == Side::BUY) {
                price = static_cast<Price_t>(stochastic_round(static_cast<double>(anchor) - dist, rng));
                price = std::max(price, MINIMUM_BID);
            } else {
                price = static_cast<Price_t>(stochastic_round(static_cast<double>(anchor) + dist, rng));
                price = std::min(price, MAXIMUM_ASK);
            }
        }

        // ------------------------------------------------------------
        // 5) Size model: mixture + urgency interaction
        // ------------------------------------------------------------
        const double near_depth =
            (side == Side::BUY && liq.has_bid_side) ? static_cast<double>(liq.bid_volumes[0]) :
            (side == Side::SELL && liq.has_ask_side) ? static_cast<double>(liq.ask_volumes[0]) : 0.0;

        const double base_scale =
            std::max(1.0, BASE_ORDER_SIZE + 0.25 * (fs.abs_volume_ewma - BASE_ORDER_SIZE));

        const double depth_factor = std::sqrt(std::min(near_depth, 200.0) + 1.0);

        // Under strong one-sided pressure, passive size can shrink, but taker size can grow.
        const double pressure = std::abs(fs.flow_imbalance);

        double urgency_factor = 1.0;
        if (regime == OrderRegime::MARKETABLE) {
            urgency_factor = 1.0 + 1.25 * urgency;
        } else if (regime == OrderRegime::IMPROVE) {
            urgency_factor = 0.9 + 0.4 * (1.0 - urgency);
        } else {
            urgency_factor = 0.9 + 0.6 * (1.0 - pressure);
        }

        const double surprise_factor = std::clamp(1.0 + fs.volume_surprise, 0.75, 1.6);

        // Add “large child order” mixture with small prob, more likely under urgency
        const double p_large = std::clamp(0.03 + 0.12 * urgency, 0.03, 0.18);
        const bool large_child = rng->bernoulli(p_large);

        const double ln_sigma = large_child ? 1.0 : 0.55;
        const double ln_mu =
            std::log(std::max(1.0, base_scale * (0.6 + 0.4 * depth_factor) * urgency_factor * surprise_factor));

        const double log_qty = ln_mu + ln_sigma * rng->standard_normal();
        const double qty_d = std::max(1.0, std::exp(log_qty));

        Volume_t qty = static_cast<Volume_t>(std::llround(qty_d));
        qty = apply_lot_clustering_(rng, qty);

        // ------------------------------------------------------------
        // 6) Cancellation threshold mass: monotone in distance + adverse selection
        // ------------------------------------------------------------
        // adverse selection proxy: if flow is going against the passive side, cancel faster
        // e.g. passive BUY cancels faster when flow_imbalance is negative.
        const double u = std::max(1e-12, rng->standard_uniform());
        const double base = -std::log(u);

        // Make deeper orders stickier: larger dist => larger mass-to-cancel
        // (k_dist calibrates how quickly stickiness grows with distance)
        constexpr double k_dist = 0.04;
        const double dist_mult = std::exp(k_dist * dist);

        // Archetype multiplier
        // MM should cancel faster => smaller threshold increment.
        // DEEP should cancel slower => larger threshold increment.
        double type_mult = 1.0;
        if (agent == AgentType::MM) type_mult = 0.55;
        else if (agent == AgentType::DEEP) type_mult = 2.0;

        // Adverse selection proxy at insertion time:
        // Passive BUY cancels faster if flow is SELL-heavy (flow_imbalance negative), and vice versa.
        const double side_flow = (side == Side::BUY ? 1.0 : -1.0);
        const double adverse = std::max(0.0, -side_flow * state.flow_state().flow_imbalance); // in [0,1]

        // Reduce mass-to-cancel under adverse selection and jump risk (withdraw liquidity faster)
        constexpr double c_adv = 2.5;
        constexpr double c_jump = 1.5;
        const double adverse_mult = 1.0 / (1.0 + c_adv * adverse + c_jump * state.vol_state().jump_intensity);

        // Final positive increment (clamped for stability)
        double hazard_increment =
            base
            * dist_mult
            * type_mult
            * adverse_mult;

        hazard_increment = std::clamp(hazard_increment, 0.02, 100.0);
        const double hazard_threshold = cumulative_hazard + hazard_increment;

        Lifespan life = Lifespan::GOOD_FOR_DAY;

        return InsertDecision{
            side,
            price,
            qty,
            life,
            hazard_threshold
        };
    }

    void update_intensity(
        const SimulationState<N>& state,
        size_t open_order_count,
        double& lambda_insert,
        double& lambda_cancel
    ) const {
        const auto& ps  = state.price_state();
        const auto& fs  = state.flow_state();
        const auto& liq = state.liq_state();
        const auto& vol = state.vol_state();

        const double vol_s = vol.realised_vol_short();
        const double spread_ticks = ps.spread ? static_cast<double>(*ps.spread) : 1.0;

        double depth_near_touch = 1.0;
        if (liq.has_bid_side && liq.has_ask_side) {
            depth_near_touch = static_cast<double>(liq.bid_volumes[0] + liq.ask_volumes[0]);
        }

        // INSERT: baseline + urgency + burstiness + thin-book refill, smoothed & capped
        double thinness = 1.0 / std::sqrt(1.0 + depth_near_touch);
        double insert_mult =
            1.0
            + 0.45 * std::abs(fs.flow_imbalance)
            + 0.65 * std::min(vol_s, 1.5)
            + 0.50 * std::clamp(fs.trade_excitation, 0.0, 3.0)
            + 0.60 * thinness;

        insert_mult = std::clamp(insert_mult, 0.3, 10.0);
        lambda_insert = LAMBDA_INSERT_BASE * insert_mult;

        // CANCEL: depends strongly on open orders, near-touch activity, volatility/jumps, and adverse selection
        double depth_mult = 0.35 + static_cast<double>(open_order_count) / CANCEL_SCALING_FACTOR;
        double vol_mult = 1.0 + 1.2 * std::min(vol_s, 1.5) + 1.0 * vol.jump_intensity;
        double flow_mult = 1.0 + 1.0 * std::abs(fs.flow_imbalance) + 0.6 * std::abs(fs.taker_sign_ewma);
        double spread_mult = 1.0 + 0.25 * spread_ticks;
        double excite_mult = 1.0 + 0.6 * std::clamp(fs.trade_excitation, 0.0, 3.0);

        double cancel_mult = depth_mult * vol_mult * flow_mult * spread_mult * excite_mult;
        cancel_mult = std::clamp(cancel_mult, 0.2, 25.0);

        lambda_cancel = LAMBDA_CANCEL_BASE * cancel_mult;
    }

private:

    static inline Price_t stochastic_round(double x, RNG* rng) {
        double f = std::floor(x);
        double frac = x - f;
        return static_cast<Price_t>(f + (rng->standard_uniform() < frac ? 1.0 : 0.0));
    }


    static inline AgentType sample_agent_(RNG* rng, double w_mm, double w_taker, double w_deep, double w_noise) {
        const double c1 = w_mm;
        const double c2 = c1 + w_taker;
        const double c3 = c2 + w_deep;
        const double u = rng->standard_uniform();

        if (u < c1) return AgentType::MM;
        if (u < c2) return AgentType::TAKER;
        if (u < c3) return AgentType::DEEP;
        return AgentType::NOISE;
    }

    static inline Price_t compute_anchor_(const PriceState& ps, const LatentState& ls, Side side, RNG* rng) {
        constexpr double best_w = 0.65;
        constexpr double fv_w   = 0.35;

        const double fv = ls.fair_value;
        const double last = static_cast<double>(ps.last_trade_price);

        if (side == Side::BUY) {
            if (ps.best_bid) {
                const double a = best_w * static_cast<double>(*ps.best_bid) + fv_w * fv;
                return static_cast<Price_t>(std::max(MINIMUM_BID, stochastic_round(a, rng)));
            }
            const double a = best_w * last + fv_w * fv;
            return static_cast<Price_t>(std::max(MINIMUM_BID, stochastic_round(a, rng)));
        } else {
            if (ps.best_ask) {
                const double a = best_w * static_cast<double>(*ps.best_ask) + fv_w * fv;
                return static_cast<Price_t>(std::max(MINIMUM_BID, stochastic_round(a, rng)));
            }
            const double a = best_w * last + fv_w * fv;
            return static_cast<Price_t>(std::max(MINIMUM_BID, stochastic_round(a, rng)));
        }
    }

    static inline Volume_t apply_lot_clustering_(RNG* rng, Volume_t q) {
        double u = rng->standard_uniform();
        if (u < 0.55) return q;

        static constexpr Volume_t lots[] = {1, 5, 10, 25, 50, 100};
        Volume_t best = lots[0];
        Volume_t best_diff = (q > best ? q - best : best - q);
        for (auto L : lots) {
            Volume_t d = (q > L ? q - L : L - q);
            if (d < best_diff) { best = L; best_diff = d; }
        }
        return std::max<Volume_t>(1, best);
    }
};
