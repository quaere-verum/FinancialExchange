#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "types.hpp"
#include "rng.hpp"
#include "state.hpp"
#include "util.hpp"



struct MetaParams {
    // Episode arrival model (hazard per second, then modulated by stress/rate)
    double base_hazard_interval = 180.0; // mean time between episodes in calm markets
    double hazard_stress_w = 0.7;        // increases start hazard with stress
    double hazard_rate_w   = 0.4;        // increases start hazard with tape speed
    double start_prob_cap  = 0.25;       // cap per-tick start probability

    // Episode duration distribution
    double mean_episode_time = 1.0;      // seconds, exponential

    // Activity mapping: how strongly META dominates when active
    double activity_when_active = 1.0;   // [0..1] (you can set <1 to soften dominance)
    double activity_decay_tau = 0.12;    // seconds: smooth activity transitions a bit

    // Placement aggressiveness
    int max_sweep_ticks = 60;
    int base_sweep_cap  = 12;

    double p_geom_calm   = 0.70;  // shallow tail
    double p_geom_urgent = 0.30;  // fatter tail (smaller p)

    // If spread is huge and urgency not extreme, sometimes post passively to reduce cost.
    double wide_spread_ticks = 6.0;
    double p_passive_when_wide = 0.65;

    // Size model
    Volume_t min_qty = 1;
    Volume_t max_qty = 5000;
    double mean_qty  = 60.0;
    double qty_sigma = 1.1;

    double urgency_size_boost = 3.0; // mean multiplier: (1 + boost * urgency)
    double surprise_size_boost = 0.8;

    // Cancel/lifespan
    double hazard_min = 0.02;
    double hazard_max = 0.50;

    // Side selection at episode start
    double side_flow_w = 0.35; // p_buy = clamp(0.5 + w*flow, ...)
    double side_pmin   = 0.10;
    double side_pmax   = 0.90;

    // Optional: reduce sweep cap when top-of-book is thin
    double thin_cap_reduction = 0.4; // multiply cap by (1 - thin_cap_reduction*thin01)
};


struct MetaState {
    bool   active = false;
    Side   side = Side::BUY;
    double time_left = 0.0;   // seconds remaining

    // Smoothed activity scalar for weighting (0..1)
    double activity = 0.0;

    // Episode-specific “style” sampled at start (gives heterogeneity)
    int    sweep_cap_ticks = 0; // per-episode cap, <= max_sweep_ticks
    double size_scale = 1.0;    // per-episode size multiplier
};


template <size_t N>
class MetaAgent {
public:
    explicit MetaAgent(MetaParams p = {}) : p_(p) {}

    const MetaState& state() const { return s_; }
    bool active() const { return s_.active; }

    // Use this in sampling weights. In [0,1].
    double activity() const { return std::clamp(s_.activity, 0.0, 1.0); }

    // Call once per tick (before set_agent_weights), using dt from SimulationState.
    void update(const SimulationState<N>& state, RNG* rng) {
        const auto ts = state.time_state();
        const auto vs = state.vol_state();
        const auto fs = state.flow_state();

        const double dt = std::max(0.0, ts.time_since_previous_sync);
        if (dt <= 0.0) return;

        // Smooth activity (prevents hard on/off in weights if desired)
        {
            const double a = 1.0 - std::exp(-dt / std::max(1e-3, p_.activity_decay_tau));
            const double target = (s_.active ? p_.activity_when_active : 0.0);
            s_.activity = (1.0 - a) * s_.activity + a * target;
            s_.activity = std::clamp(s_.activity, 0.0, 1.0);
        }

        // If active, decrement time_left and possibly turn off.
        if (s_.active) {
            s_.time_left -= dt;
            if (s_.time_left <= 0.0) {
                s_.active = false;
                s_.time_left = 0.0;
                // keep s_.activity smoothing to decay; keep style vars as last-used
            }
            return;
        }

        // Not active: hazard of starting an episode
        // Stress proxy: log(vol_short / vol_long), positive part
        const double vol_s = vs.realised_vol_short;
        const double vol_l = vs.realised_vol_long;
        const double stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);

        // Tape speed proxy: short/long trade-rate ratio if you have it; otherwise use ewma
        double rate_n = std::clamp(fs.trade_rate_ewma_short / (fs.trade_rate_ewma_long + 1e-6), 0.0, 3.0);

        double hazard = 1.0 / std::max(1e-6, p_.base_hazard_interval);
        hazard *= (1.0 + p_.hazard_stress_w * std::max(0.0, stress));
        hazard *= (1.0 + p_.hazard_rate_w   * rate_n);

        const double p_start = std::clamp(hazard * dt, 0.0, p_.start_prob_cap);
        if (rng->standard_uniform() < p_start) {
            start_episode_(state, rng);
        }
    }

    // Generate a META insert. Call only when sampled as META.
    InsertDecision decide_insert(const SimulationState<N>& state, double cumulative_hazard, RNG* rng) const {
        const auto ps = state.price_state();
        const auto ls = state.liq_state();
        const auto vs = state.vol_state();
        const auto fs = state.flow_state();

        // If somehow sampled while inactive, just act like a moderately aggressive taker.
        const Side side = s_.side;

        // Urgency rises as time_left shrinks. Use mean_episode_time as scale.
        const double Tref = std::max(1e-6, p_.mean_episode_time);
        const double urgency = std::clamp(1.0 - (s_.time_left / Tref), 0.0, 1.0);

        // Fallback if no BBO
        if (!ps.best_bid || !ps.best_ask) {
            const Price_t ref = ps.last_trade_price;
            const int k = std::min(5, p_.max_sweep_ticks);
            const Price_t px = (side == Side::BUY) ? (ref + (Price_t)(k))
                                                   : (ref - (Price_t)(k));

            const double mean = p_.mean_qty * s_.size_scale * (1.0 + p_.urgency_size_boost * urgency);
            const double z = rng->standard_normal();
            const double base = std::max(1e-6, p_.mean_qty);
            double q = base * std::exp(p_.qty_sigma * z);
            q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);

            return { side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p_.hazard_min + cumulative_hazard};
        }

        const Price_t bb = *ps.best_bid;
        const Price_t ba = *ps.best_ask;
        const double spread_ticks = (double)(ba - bb);

        // Stress proxy again for aggressiveness scaling
        const double vol_s = vs.realised_vol_short;
        const double vol_l = vs.realised_vol_long;
        const double stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);
        const double stress01 = std::clamp(std::max(0.0, stress) / 1.5, 0.0, 1.0);

        // Thinness from top bucket depth
        const double depth0 = (double)ls.bid_volumes[0] + (double)ls.ask_volumes[0];
        const double thin01 = std::clamp(1.0 - std::log(depth0 + 1.0) / 6.0, 0.0, 1.0);

        // Passive fallback when spread is huge and urgency not extreme
        bool passive = false;
        if (spread_ticks >= p_.wide_spread_ticks) {
            const double p_passive = std::clamp(p_.p_passive_when_wide * (1.0 - 0.7 * urgency), 0.05, 0.95);
            passive = (rng->standard_uniform() < p_passive);
        }

        Price_t px;
        if (!passive) {
            // Sweep cap: per-episode base + urgency/stress scaling; reduced when thin.
            int cap = s_.sweep_cap_ticks > 0 ? s_.sweep_cap_ticks : p_.base_sweep_cap;
            cap = (int)std::llround(cap * (1.0 + 2.0 * urgency + 0.8 * stress01));
            cap = std::clamp(cap, 1, p_.max_sweep_ticks);
            cap = (int)std::llround(cap * (1.0 - p_.thin_cap_reduction * thin01));
            cap = std::clamp(cap, 1, p_.max_sweep_ticks);

            // Geometric parameter interpolated by urgency (urgent => smaller p => fatter tail)
            const double p_geom = p_.p_geom_calm + (p_.p_geom_urgent - p_.p_geom_calm) * urgency;
            const int k = sample_geometric_offset(rng, p_geom, cap);

            px = (side == Side::BUY) ? (ba + (Price_t)(k))
                                     : (bb - (Price_t)(k));
        } else {
            // Passive: join best, sometimes improve by 1 tick if spread allows and urgency moderate
            if (side == Side::BUY) {
                px = bb;
                if (spread_ticks >= 2.0 && rng->standard_uniform() < (0.15 + 0.25 * urgency)) {
                    px = bb + 1;
                }
            } else {
                px = ba;
                if (spread_ticks >= 2.0 && rng->standard_uniform() < (0.15 + 0.25 * urgency)) {
                    px = ba - 1;
                }
            }
        }

        // Size: heavy tail + urgency + volume surprise
        const double surp01 = std::clamp(fs.volume_surprise, 0.0, 2.0) / 2.0;
        double mean = p_.mean_qty * s_.size_scale
                    * (1.0 + p_.urgency_size_boost * urgency)
                    * (1.0 + p_.surprise_size_boost * surp01);

        if (spread_ticks >= p_.wide_spread_ticks && !passive && urgency < 0.5) mean *= 0.6;

        const double z = rng->standard_normal();
        const double base = std::max(1e-6, p_.mean_qty);
        double q = base * std::exp(p_.qty_sigma * z);
        q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);
        const Volume_t qty = (Volume_t)std::llround(q);

        // Hazard: urgent => low cancels
        double hazard = p_.hazard_max - (p_.hazard_max - p_.hazard_min) * urgency;
        hazard = std::clamp(hazard, p_.hazard_min, p_.hazard_max);

        Lifespan lifespan = Lifespan::GOOD_FOR_DAY;

        return { side, px, qty, lifespan, hazard + cumulative_hazard };
    }

private:
    void start_episode_(const SimulationState<N>& state, RNG* rng) {
        const auto fs = state.flow_state();

        s_.active = true;

        // Duration
        s_.time_left = rng->exponential(1.0 / std::max(1e-6, p_.mean_episode_time));

        // Side from flow imbalance (directional)
        const double flow = std::clamp(fs.flow_imbalance, -1.0, 1.0);
        const double p_buy = std::clamp(0.5 + p_.side_flow_w * flow, p_.side_pmin, p_.side_pmax);
        s_.side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

        // Episode “style” heterogeneity
        // Sweep cap: lognormal-ish around base_sweep_cap, clipped
        {
            const double z = rng->standard_normal();
            double cap = (double)p_.base_sweep_cap * std::exp(0.35 * z);
            cap = std::clamp(cap, 1.0, (double)p_.max_sweep_ticks);
            s_.sweep_cap_ticks = (int)std::llround(cap);
        }

        // Size scale: mild heterogeneity
        {
            const double z = rng->standard_normal();
            double sc = std::exp(0.25 * z);
            s_.size_scale = std::clamp(sc, 0.6, 2.0);
        }
    }

private:
    MetaParams p_;
    MetaState  s_;
};
