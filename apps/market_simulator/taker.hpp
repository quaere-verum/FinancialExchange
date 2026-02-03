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

struct TakerState {
    double urgency = 0.0;
    double bias    = 0.0;
};


class TakerAgent {
public:
    explicit TakerAgent(TakerParams p = {}) : p_(p) {}

    TakerState& state() { return s_; }
    const TakerState& state() const { return s_; }

    void update(const FeatureVector& state) {
        const double dt = std::max(0.0, state.time_since_previous_sync);
        if (dt <= 0.0) return;

        const double a_u = 1.0 - std::exp(-dt / std::max(1e-3, p_.tau_urgency));
        const double a_b = 1.0 - std::exp(-dt / std::max(1e-3, p_.tau_bias));

        double u_target =
            p_.w_flow_abs * std::fabs(state.flow_imbalance) +
            p_.w_surprise * state.surprise_signx01 +
            p_.w_stress   * state.stress01 +
            p_.w_jump     * state.jump01;

        u_target = std::clamp(u_target, 0.0, 1.0);
        s_.urgency = (1.0 - a_u) * s_.urgency + a_u * u_target;
        double b_target = p_.w_flow_sign * state.flow_imbalance + p_.w_fair * state.mid_fair_gap;
        // When not urgent, bias should mean-revert faster (uninformed pokes)
        const double urgency = s_.urgency;
        const double b_gain = (0.35 + 0.65 * urgency); // [0.35,1.0]
        s_.bias = (1.0 - a_b) * s_.bias + a_b * (b_gain * b_target);
        s_.bias = std::clamp(s_.bias, -1.0, 1.0);
    }

    inline void decide_insert(
        const FeatureVector& state,
        double cumulative_hazard,
        RNG* rng,
        std::vector<InsertDecision>& insert_decisions
    ) const {
        if (!state.has_ask_side || !state.has_bid_side) {
            const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
            const Price_t ref = state.last_trade_price;
            const Price_t offset = sample_geometric_offset(rng, 0.20, 10);
            const Price_t px = (side == Side::BUY) ? (ref - offset) : (ref + offset);
            insert_decisions.push_back({ side, px, (Volume_t)1, Lifespan::GOOD_FOR_DAY, p_.hazard_max + cumulative_hazard });
            return;
        }

        const double urgency = s_.urgency;
        const double bias = s_.bias;

        // --- Side choice: bias dominates when urgent, noise dominates when not urgent ---
        const double bias_weight = 0.35 + 0.65 * urgency;              // [0.35,1.0]
        const double p_buy = std::clamp(0.5 + 0.45 * (bias_weight * bias), 0.05, 0.95);
        const Side side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

        // --- Size ---
        const double z = rng->standard_normal();
        double q = std::max(1e-6, p_.mean_qty) * std::exp(p_.qty_sigma * z);
        q *= (1.0 + p_.urgency_size_boost * urgency);
        q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);
        const Volume_t qty = (Volume_t)std::llround(q);

        // Cross more when urgent AND either tight OR thin (thin -> grab before it vanishes)
        double p_cross = p_.p_cross_base +
            (p_.p_cross_max - p_.p_cross_base) * urgency * (0.45 + 0.35 * state.tight01 + 0.20 * state.thin01);
        p_cross = std::clamp(p_cross, 0.0, 0.95);

        double p_improve = 0.08 * urgency * (1.0 - 0.7 * state.thin01);
        if (state.spread < 2) p_improve = 0.0;

        double p_join = std::max(p_.p_join_best_floor, 1.0 - p_cross - p_improve);

        const double Z = p_cross + p_improve + p_join;
        p_cross /= Z; p_improve /= Z; p_join /= Z;

        // --- Choose action and price ---
        const double u = rng->standard_uniform();
        Price_t px;

        if (u < p_cross) {
            // Depth-aware "sweep": choose how many levels to cross so expected fill >= qty
            const size_t levels = choose_sweep_levels_(side, qty, state, urgency, rng);

            // Marketable limit price: last swept level (prevents absurd ba+10 jumps)
            // Assumes tick size = 1 and contiguous levels. If your book is sparse, map levels to price ladder.
            px = (side == Side::BUY) ? (Price_t)(*state.best_ask + (Price_t)(levels ? (levels - 1) : 0))
                                    : (Price_t)(*state.best_bid - (Price_t)(levels ? (levels - 1) : 0));
        } else if (u < p_cross + p_improve) {
            px = (side == Side::BUY) ? (Price_t)(*state.best_bid + 1) : (Price_t)(*state.best_ask - 1);
        } else {
            px = (side == Side::BUY) ? *state.best_bid : *state.best_ask;
        }

        // --- Hazard mass ---
        // Interpret taker as IOC-like when urgent: if not filled quickly, cancel quickly => small mass
        // Non-urgent join/improve orders can rest longer => larger mass
        double mass = p_.hazard_max - (p_.hazard_max - p_.hazard_min) * urgency; // urgent -> small
        mass = std::clamp(mass, p_.hazard_min, p_.hazard_max);

        insert_decisions.push_back({ side, px, qty, Lifespan::GOOD_FOR_DAY, cumulative_hazard + mass });
    }


private:
    TakerParams p_;
    TakerState  s_;

    inline Price_t sample_aggressive_offset_ticks(double urgency, RNG* rng) const {
        // higher urgency => higher chance of deeper sweep
        double p = 0.70 - 0.45 * urgency;      // p in [0.25, 0.70]
        p = std::clamp(p, 0.20, 0.80);
        Price_t k = 0;
        while (k < 10 && rng->standard_uniform() > p) ++k; // geometric tail
        return k; // 0..10
    }

    inline size_t choose_sweep_levels_(
        Side side,
        Volume_t qty,
        const FeatureVector& state,
        double urgency,
        RNG* rng
    ) const {
        // Try to sweep enough levels so that cumulative displayed volume >= qty
        // But allow randomness and urgency dependence (more urgent => more likely to sweep deeper).
        Volume_t cum = 0;
        size_t lvl = 0;

        const size_t max_lvl = 1; // Need to extend state before sweeping more levels
        const double slack = 0.85 + 0.30 * urgency;             // urgent tolerates less shortfall

        while (lvl < max_lvl && (double)cum < slack * (double)qty) {
            const Volume_t v = (side == Side::BUY) ? state.ask_at_touch : state.bid_at_touch;
            cum += v;
            ++lvl;
            // Random early stop: sometimes they don't fully sweep displayed depth
            if (lvl >= 2 && rng->standard_uniform() < (0.15 * (1.0 - urgency))) break;
        }

        return std::max<size_t>(1, lvl);
    }

};
