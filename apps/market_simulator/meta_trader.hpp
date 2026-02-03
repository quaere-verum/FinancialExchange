#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "types.hpp"
#include "rng.hpp"
#include "state.hpp"
#include "util.hpp"
#include "parameters.hpp"


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


class MetaAgent {
public:
    explicit MetaAgent(MetaParams p = {}) : p_(p) {}

    const MetaState& state() const { return s_; }
    bool active() const { return s_.active; }

    // Use this in sampling weights. In [0,1].
    double activity() const { return std::clamp(s_.activity, 0.0, 1.0); }

    // Call once per tick (before set_agent_weights), using dt from SimulationState.
    void update(const FeatureVector& state, RNG* rng) {
        const double dt = std::max(0.0, state.time_since_previous_sync);
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

        double hazard = 1.0 / std::max(1e-6, p_.base_hazard_interval);
        hazard *= (1.0 + p_.hazard_stress_w * std::max(0.0, state.stress01));
        hazard *= (1.0 + p_.hazard_rate_w   * state.trade_rate01);

        const double p_start = std::clamp(hazard * dt, 0.0, p_.start_prob_cap);
        if (rng->standard_uniform() < p_start) {
            start_episode_(state, rng);
        }
    }

    // Generate a META insert. Call only when sampled as META.
    inline void decide_insert(
        const FeatureVector& state, 
        double cumulative_hazard, 
        RNG* rng,
        std::vector<InsertDecision>& insert_decisions
    ) const {
        // If somehow sampled while inactive, just act like a moderately aggressive taker.
        const Side side = s_.side;

        // Urgency rises as time_left shrinks. Use mean_episode_time as scale.
        const double Tref = std::max(1e-6, p_.mean_episode_time);
        const double urgency = std::clamp(1.0 - (s_.time_left / Tref), 0.0, 1.0);

        // Fallback if no BBO
        if (!state.has_bid_side || !state.has_ask_side) {
            const Price_t ref = state.last_trade_price;
            const int k = std::min(5, p_.max_sweep_ticks);
            const Price_t px = (side == Side::BUY) ? (ref + (Price_t)(k))
                                                   : (ref - (Price_t)(k));

            const double mean = p_.mean_qty * s_.size_scale * (1.0 + p_.urgency_size_boost * urgency);
            const double z = rng->standard_normal();
            const double base = std::max(1e-6, p_.mean_qty);
            double q = base * std::exp(p_.qty_sigma * z);
            q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);

            insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p_.hazard_min + cumulative_hazard});
            return;
        }

        // Passive fallback when spread is huge and urgency not extreme
        bool passive = false;
        if (state.spread >= p_.wide_spread_ticks) {
            const double p_passive = std::clamp(p_.p_passive_when_wide * (1.0 - 0.7 * urgency), 0.05, 0.95);
            passive = (rng->standard_uniform() < p_passive);
        }

        Price_t px;
        if (!passive) {
            // Sweep cap: per-episode base + urgency/stress scaling; reduced when thin.
            int cap = s_.sweep_cap_ticks > 0 ? s_.sweep_cap_ticks : p_.base_sweep_cap;
            cap = (int)std::llround(cap * (1.0 + 2.0 * urgency + 0.8 * state.stress01));
            cap = std::clamp(cap, 1, p_.max_sweep_ticks);
            cap = (int)std::llround(cap * (1.0 - p_.thin_cap_reduction * state.thin01));
            cap = std::clamp(cap, 1, p_.max_sweep_ticks);

            // Geometric parameter interpolated by urgency (urgent => smaller p => fatter tail)
            const double p_geom = p_.p_geom_calm + (p_.p_geom_urgent - p_.p_geom_calm) * urgency;
            const int k = sample_geometric_offset(rng, p_geom, cap);

            px = (side == Side::BUY) ? (*state.best_ask + (Price_t)(k))
                                     : (*state.best_bid - (Price_t)(k));
        } else {
            // Passive: join best, sometimes improve by 1 tick if spread allows and urgency moderate
            if (side == Side::BUY) {
                px = *state.best_bid;
                if (state.spread >= 2.0 && rng->standard_uniform() < (0.15 + 0.25 * urgency)) {
                    px = *state.best_bid + 1;
                }
            } else {
                px = *state.best_ask;
                if (state.spread >= 2.0 && rng->standard_uniform() < (0.15 + 0.25 * urgency)) {
                    px = *state.best_ask - 1;
                }
            }
        }

        double mean = p_.mean_qty * s_.size_scale
                    * (1.0 + p_.urgency_size_boost * urgency)
                    * (1.0 + p_.surprise_size_boost * std::max(state.surprise_signx01, 0.0));

        if (state.spread >= p_.wide_spread_ticks && !passive && urgency < 0.5) mean *= 0.6;

        const double z = rng->standard_normal();
        const double base = std::max(1e-6, p_.mean_qty);
        double q = base * std::exp(p_.qty_sigma * z);
        q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);
        const Volume_t qty = (Volume_t)std::llround(q);

        // Hazard: urgent => low cancels
        double hazard = p_.hazard_max - (p_.hazard_max - p_.hazard_min) * urgency;
        hazard = std::clamp(hazard, p_.hazard_min, p_.hazard_max);

        Lifespan lifespan = Lifespan::GOOD_FOR_DAY;

        insert_decisions.push_back({ side, px, qty, lifespan, hazard + cumulative_hazard });
        return;
    }

private:
    void start_episode_(const FeatureVector& state, RNG* rng) {
        s_.active = true;

        // Duration
        s_.time_left = rng->exponential(1.0 / std::max(1e-6, p_.mean_episode_time));

        // Side from flow imbalance (directional)
        const double flow = std::clamp(state.flow_imbalance, -1.0, 1.0);
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
