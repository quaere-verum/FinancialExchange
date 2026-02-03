#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "types.hpp"
#include "util.hpp"
#include "rng.hpp"
#include "state.hpp"
#include "parameters.hpp"


struct MarketMakerLatentState {
    double risk = 0.0;
    double bias = 0.0;
    double log_depth0_ref = 0.0;
    bool depth_ref_init = false;
};

class MarketMakerAgent {
    public:
        explicit MarketMakerAgent(MarketMakerParams p = {}) : p_(p) {}

        MarketMakerLatentState& state() { return s_; }
        const MarketMakerLatentState& state() const { return s_; }

        void update(
            const FeatureVector& state
        ) {
            const double dt = std::max(0.0, state.time_since_previous_sync);
            if (dt <= 0.0) return;

            const double logd0 = std::log(state.depth_at_touch + 1.0);
            if (!s_.depth_ref_init) {
                s_.log_depth0_ref = logd0;
                s_.depth_ref_init = true;
            }
            // baseline update (slow)
            {
                constexpr double TAU_DEPTH_REF = 5.0; // seconds
                const double a = 1.0 - std::exp(-dt / TAU_DEPTH_REF);
                s_.log_depth0_ref = (1.0 - a) * s_.log_depth0_ref + a * logd0;
            }

            // --- RISK update (EWMA to target) ---
            constexpr double TAU_RISK = 0.5; // seconds (fast-ish)
            const double a_r = 1.0 - std::exp(-dt / TAU_RISK);

            // const double r_target = std::clamp(
            //     0.10 * state.spread01 +
            //     0.40 * 0.5 * std::tanh(std::log(std::max(state.vol_ratio, EPS)) + 1.0) +
            //     0.35 * std::fabs(state.flow_imbalance) +
            //     0.20 * std::max(state.surprise_signx01, 0.0) +
            //     0.30 * state.thin01,
            //     0.0, 1.0
            // );
            // std::cout 
            //     << "spread01=" << state.spread01
            //     << ", vol_ratio=" << 0.5 * std::tanh(std::log(std::max(state.vol_ratio, EPS)) + 1.0)
            //     << ", abs_flow_imb=" << std::fabs(state.flow_imbalance)
            //     << ", surprise=" << std::max(state.surprise_signx01, 0.0)
            //     << ", thin01=" << state.thin01
            //     << ", r_target=" << r_target
            //     << "\n";
            const double r_target = state.stress_micro01;

            s_.risk = (1.0 - a_r) * s_.risk + a_r * r_target;
            s_.risk = std::clamp(s_.risk, 0.0, 1.0);

            // --- BIAS update (OU to target) ---
            constexpr double TAU_BIAS = 1.2; // seconds (slower)
            const double a_b = 1.0 - std::exp(-dt / TAU_BIAS);
            const double b_flow = 0.20 * (-state.flow_imbalance) * (1.0 - 0.5 * s_.risk);
            const double b_target = std::clamp(
                b_flow +              // contrarian to taker pressure
                0.25 * (-state.flow_imbalance) +      // lean against local book imbalance
                0.15 * (state.mid_fair_gap),    // weak anchor
                -1.0, 1.0
            );

            s_.bias = (1.0 - a_b) * s_.bias + a_b * b_target;

            // Damp leaning when risk is high (makers stop “positioning” and reduce exposure)
            s_.bias *= (1.0 - 0.6 * s_.risk);
            s_.bias = std::clamp(s_.bias, -1.0, 1.0);
        }

        inline void decide_insert(
            const FeatureVector& state,
            double cumulative_hazard,
            RNG* rng,
            std::vector<InsertDecision>& insert_decisions
        ) {
            constexpr size_t half = MM_LADDER_SIZE / 2;
            const Lifespan lifespan = Lifespan::GOOD_FOR_DAY;

            if (!state.has_ask_side || !state.has_bid_side) {
                // fallback: still stochastic levels, and hazard decreases with risk (faster cancels)
                const Price_t ref = state.last_trade_price;

                // choose how many levels per side
                const size_t levels = std::min(half, sample_levels_(rng));

                // base distance increases with risk; add randomness
                Price_t base_d = (Price_t)std::clamp((int)std::llround(2.0 + 8.0 * s_.risk), 2, 12);
                base_d += (Price_t)sample_geometric_offset(rng, 0.6, 3);

                for (size_t lvl = 0; lvl < levels; ++lvl) {
                    const Price_t d = base_d + (Price_t)lvl;

                    const double depth_decay = std::exp(-0.35 * (double)lvl);
                    Volume_t qty_bid = (Volume_t)std::clamp(
                        (int)std::llround((double)sample_qty_(rng) * depth_decay),
                        (int)p_.min_qty, (int)p_.max_qty
                    );
                    Volume_t qty_ask = (Volume_t)std::clamp(
                        (int)std::llround((double)sample_qty_(rng) * depth_decay),
                        (int)p_.min_qty, (int)p_.max_qty
                    );

                    const double mass = choose_hazard_mass_(state.depth_at_touch, lvl);
                    const double hazard_threshold = cumulative_hazard + mass;

                    if (qty_bid > 0) insert_decisions.push_back({ Side::BUY,  (Price_t)(ref - d), qty_bid, lifespan, hazard_threshold });
                    if (qty_ask > 0) insert_decisions.push_back({ Side::SELL, (Price_t)(ref + d), qty_ask, lifespan, hazard_threshold });
                }
                return;
            }
            const bool one_sided = rng->standard_uniform() <
                (p_.p_one_sided_when_stressed * state.stress01);

            const Side preferred = choose_side_(state.imbalance_at_touch, state.flow_imbalance, state.mid_fair_gap, rng);

            // --- decide join vs backoff at touch ---
            double p_join = p_.p_join_touch_base * (1.0 - p_.join_risk_slope * s_.risk);
            p_join = std::clamp(p_join, 0.0, 1.0);

            const bool join_touch = (rng->standard_uniform() < p_join);
            const Price_t backoff = join_touch ? (Price_t)0 : sample_backoff_(rng);

            // --- compute top-of-book prices using improve/join/backoff ---
            // reference mid (you can later blend in fair_value when risk low)
            const Price_t ref_mid = clamp_mid_(*state.best_bid, *state.best_ask, state.fair_value);

            // discrete-but-not-quantized skew
            const Price_t skew = sample_tick_skew_(rng);

            // base half-spread target (still OK)
            const Price_t half_sp = target_half_spread_ticks_(state, rng);

            Price_t bid0 = (Price_t)(ref_mid - half_sp + skew);
            Price_t ask0 = (Price_t)(ref_mid + half_sp + skew);

            // Do not cross
            bid0 = std::min<Price_t>(bid0, (Price_t)(*state.best_ask - 1));
            ask0 = std::max<Price_t>(ask0, (Price_t)(*state.best_bid + 1));

            // Decide improve vs join when wide
            // Improve only if spread>=2 and risk low-ish
            const bool can_improve = (state.spread >= 2.0);
            double p_improve = p_.p_improve_when_wide;
            p_improve *= (1.0 - p_.improve_risk_slope * s_.risk);
            p_improve = std::clamp(p_improve, 0.0, 1.0);

            const bool improve = can_improve && join_touch && (rng->standard_uniform() < p_improve);

            if (improve) {
                // step inside
                bid0 = (Price_t)std::min<Price_t>((Price_t)(*state.best_bid + 1), (Price_t)(*state.best_ask - 1));
                ask0 = (Price_t)std::max<Price_t>((Price_t)(*state.best_ask - 1), (Price_t)(*state.best_bid + 1));
                // note: above two lines keep you inside without crossing even if spread==2
                // if spread==2, bb+1 == ba-1, that’s the only inside tick
            } else if (join_touch) {
                // join touch, but don’t worsen
                bid0 = std::max<Price_t>(bid0, *state.best_bid);
                ask0 = std::min<Price_t>(ask0, *state.best_ask);
            } else {
                // back off from touch
                bid0 = (Price_t)std::min<Price_t>(bid0, (Price_t)(*state.best_bid - backoff));
                ask0 = (Price_t)std::max<Price_t>(ask0, (Price_t)(*state.best_ask + backoff));
            }

            // Ladder step: larger in stress/risk
            Price_t step = (Price_t)std::clamp((int)std::llround(1.0 + 3.0 * s_.risk), 1, 6);

            // How many levels to post (per side)
            const size_t levels = std::min(half, sample_levels_(rng));

            // Base sizes per side, decay with depth and risk
            const Volume_t qty0_bid = sample_qty_(rng);
            const Volume_t qty0_ask = sample_qty_(rng);
            const double risk_decay0 = (1.0 - 0.55 * s_.risk);

            for (size_t lvl = 0; lvl < levels; ++lvl) {
                const Price_t px_bid = (Price_t)(bid0 - (Price_t)lvl * step);
                const Price_t px_ask = (Price_t)(ask0 + (Price_t)lvl * step);

                const double depth_decay = std::exp(-0.45 * (double)lvl);

                Volume_t qty_bid = (Volume_t)std::clamp(
                    (int)std::llround((double)qty0_bid * depth_decay * risk_decay0),
                    (int)p_.min_qty, (int)p_.max_qty
                );
                Volume_t qty_ask = (Volume_t)std::clamp(
                    (int)std::llround((double)qty0_ask * depth_decay * risk_decay0),
                    (int)p_.min_qty, (int)p_.max_qty
                );

                // One-sided: skip the inactive side entirely
                if (one_sided) {
                    if (preferred != Side::BUY)  qty_bid = 0;
                    if (preferred != Side::SELL) qty_ask = 0;
                }

                // Level-specific hazard thresholds (touch cancels faster; deep cancels slower)
                const double mass = choose_hazard_mass_(state.depth_at_touch, lvl);
                const double hazard_threshold = cumulative_hazard + mass;

                if (qty_bid > 0) insert_decisions.push_back({ Side::BUY,  px_bid, qty_bid, lifespan, hazard_threshold });
                if (qty_ask > 0) insert_decisions.push_back({ Side::SELL, px_ask, qty_ask, lifespan, hazard_threshold });
            }
        }

    private:
        MarketMakerParams p_;
        MarketMakerLatentState s_;

        inline Side choose_side_(double imb0, double flow, double fair_gap, RNG* rng) {
            // Small optional "fresh" correction (keep coefficients small!)
            const double corr =
                0.05 * (-imb0) +      // lean against local book imbalance
                0.05 * (fair_gap);    // weak anchor

            // risk reduces leaning / makes quoting more symmetric
            const double eff_bias = (s_.bias + corr) * (1.0 - 0.5 * s_.risk);

            const double p_buy = std::clamp(0.5 + 0.40 * eff_bias, 0.05, 0.95);
            return (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;
        }

        inline Price_t choose_price_(
            Side side,
            Price_t bb,
            Price_t ba,
            double spread_ticks,
            double depth0,
            RNG* rng
        ) const {
            const bool can_improve = (spread_ticks >= 2.0);

            const double logd0 = std::log(depth0 + 1.0);
            const double thin_raw = std::clamp(s_.log_depth0_ref - logd0, 0.0, 2.0);
            const double thin01 = thin_raw / 2.0; // 0..1

            double p_improve = p_.p_improve_when_wide;

            // risk/thin reduce the probability to step inside (avoid being picked off)
            p_improve *= (1.0 - 0.70 * s_.risk);
            p_improve *= (1.0 - 0.35 * thin01);
            p_improve = std::clamp(p_improve, 0.01, 0.90);

            if (can_improve && (rng->standard_uniform() < p_improve)) {
                return (side == Side::BUY) ? (bb + 1) : (ba - 1);
            }

            return (side == Side::BUY) ? bb : ba;
        }

        inline Volume_t sample_qty_(RNG* rng) const {

            const double z = rng->standard_normal();
            const double base = std::max(1e-6, p_.mean_qty);
            double q = base * std::exp(p_.qty_logn_sigma * z);
            q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);
            return (Volume_t)std::llround(q);
        }

        inline double choose_hazard_mass_(double depth0, size_t lvl) const {
            const double logd0 = std::log(depth0 + 1.0);
            const double thin_raw = std::clamp(s_.log_depth0_ref - logd0, 0.0, 2.0);
            const double thin01 = thin_raw / 2.0;

            // start from baseline
            double m = p_.base_hazard_mass;

            // IMPORTANT: lower mass => faster cancel.
            // So in stress/thinness we DECREASE mass (shorter lifetime).
            m *= (1.0 - p_.hazard_stress_strength * 0.70 * s_.risk);     // up to ~70% reduction
            m *= (1.0 - p_.hazard_thin_strength   * 0.60 * thin01);      // further reduction when thin

            // deeper levels live longer: increase mass by level
            m *= (1.0 + p_.hazard_level_slope * (double)lvl);

            return std::clamp(m, p_.hazard_mass_min, p_.hazard_mass_max);
        }


        inline Price_t clamp_mid_(Price_t bb, Price_t ba, double fair_value) const {
            // Fair can be outside the book; keep ref mid within [bb, ba] if possible.
            const double mid = 0.5 * (double)(bb + ba);
            const double ref = 1.0 * mid + 0.0 * fair_value;
            return std::llround(ref);
        }

        inline Price_t target_half_spread_ticks_(const FeatureVector& state, RNG* rng) const {
            // Baseline: 1 tick, widen smoothly with risk & thinness
            double z = std::clamp(0.7 * state.stress_micro01 + 0.3 * state.thin01, 0.0, 1.0);
            z = std::pow(z, MICRO_GAMMA);

            double p_calm = 0.90;
            double p_stress = 0.25;

            double p = p_calm - (p_calm - p_stress) * z;   // decreases with stress
            Price_t half_spread = 1 + sample_geometric_offset(rng, p, 10);
            return half_spread;
        }

        inline Price_t sample_tick_skew_(RNG* rng) const {
            // bias in [-1,1]; apply a 1-tick skew with probability |bias|
            const double b = std::clamp(s_.bias, -1.0, 1.0);
            const double u = rng->standard_uniform();
            if (u < std::abs(b)) return (b > 0.0) ? (Price_t)1 : (Price_t)-1;
            return (Price_t)0;
        }

        inline size_t sample_levels_(RNG* rng) const {
            // interpolate expected levels between calm and stress
            const double mean = (1.0 - s_.risk) * p_.ladder_level_mean_calm + s_.risk * p_.ladder_level_mean_stress;

            // simple bounded Poisson-like sampling via geometric-ish method:
            // sample k by counting uniforms until product < exp(-mean)
            size_t k = 0;
            double L = std::exp(-mean);
            double prod = 1.0;
            while (prod > L && k < p_.ladder_levels_max) {
                prod *= rng->standard_uniform();
                ++k;
            }
            k = std::clamp(k, p_.ladder_levels_min, p_.ladder_levels_max);
            return k;
        }

        inline Price_t sample_backoff_(RNG* rng) const {
            // backoff in ticks when not joining. Increases with risk.
            const double max_b = (double)p_.backoff_ticks_max;
            const double m = std::clamp(0.5 + 2.5 * s_.risk, 0.5, max_b);
            // geometric-ish small integers, capped
            Price_t b = (Price_t)std::clamp((int)std::llround(sample_geometric_offset(rng, 0.55, (int)max_b)), 0, (int)p_.backoff_ticks_max);
            // bias toward larger backoff when risk high
            if (rng->standard_uniform() < s_.risk) b = (Price_t)std::min<int>((int)p_.backoff_ticks_max, (int)(b + 1));
            return b;
        }


};
