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

struct TakerParams {
    // Latent dynamics
    double tau_urgency = 0.30;   // seconds (fast)
    double tau_bias    = 0.80;   // seconds (slower)

    // How state features map into urgency/bias targets
    double w_flow_abs  = 0.65;   // |flow_imbalance| -> urgency
    double w_surprise  = 0.25;   // volume_surprise -> urgency
    double w_stress    = 0.35;   // vol_stress_pos  -> urgency
    double w_jump      = 0.15;   // jump_intensity  -> urgency (optional)

    double w_flow_sign = 0.90;   // signed flow -> bias target
    double w_fair      = 0.10;   // fair-mid gap -> bias target (weak)

    // Aggressiveness model
    double p_cross_base = 0.05;   // baseline chance to cross spread
    double p_cross_max  = 0.70;   // at urgency=1 and tight spreads
    double p_join_best_floor = 0.20; // minimum join-best probability

    // Sizes
    Volume_t min_qty = 1;
    Volume_t max_qty = 200;
    double mean_qty  = 6.0;
    double qty_sigma = 0.9;      // lognormal tail
    double urgency_size_boost = 1.2; // urgency increases size

    // For taker-like flow, cancels should be relatively *high* when not urgent,
    // and *low* when urgent (they want execution).
    double hazard_min = 0.05;
    double hazard_max = 1.50;
};

struct TakerState {
    double urgency = 0.0;
    double bias    = 0.0;
};

template <size_t N>
class TakerAgent {
public:
    explicit TakerAgent(TakerParams p = {}) : p_(p) {}

    TakerState& state() { return s_; }
    const TakerState& state() const { return s_; }

    // Call once per tick (before generating a batch is fine)
    void update(const SimulationState<N>& state) {
        const auto ts = state.time_state();
        const auto vs = state.vol_state();
        const auto fs = state.flow_state();
        const auto ps = state.price_state();
        const auto lat = state.latent_state();

        const double dt = std::max(0.0, ts.time_since_previous_sync);
        if (dt <= 0.0) return;

        const double a_u = 1.0 - std::exp(-dt / std::max(1e-3, p_.tau_urgency));
        const double a_b = 1.0 - std::exp(-dt / std::max(1e-3, p_.tau_bias));

        const double flow = fs.flow_imbalance;
        const double absflow = std::abs(flow);

        const double surp01 = std::clamp(fs.volume_surprise, 0.0, 2.0) / 2.0;

        const double vol_s = vs.realised_vol_short;
        const double vol_l = vs.realised_vol_long;
        const double vol_stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);
        const double stress01 = std::clamp(std::max(0.0, vol_stress) / 1.5, 0.0, 1.0);

        const double jump01 = std::clamp(vs.jump_intensity, 0.0, 1.0);

        double u_target =
            p_.w_flow_abs * absflow +
            p_.w_surprise * surp01 +
            p_.w_stress   * stress01 +
            p_.w_jump     * jump01;
        u_target = std::clamp(u_target, 0.0, 1.0);

        s_.urgency = (1.0 - a_u) * s_.urgency + a_u * u_target;

        // Bias target in [-1,1]
        const double mid =
            (ps.best_bid && ps.best_ask) ? 0.5 * ((double)*ps.best_bid + (double)*ps.best_ask)
                                         : (double)ps.last_trade_price;
        const double fair_gap = std::clamp((lat.fair_value - mid) / std::max(1.0, mid), -0.02, 0.02) / 0.02;

        double b_target = p_.w_flow_sign * flow + p_.w_fair * fair_gap;

        // When not urgent, bias should mean-revert faster (uninformed pokes)
        const double urgency = s_.urgency;
        const double b_gain = (0.35 + 0.65 * urgency); // [0.35,1.0]
        s_.bias = (1.0 - a_b) * s_.bias + a_b * (b_gain * b_target);
        s_.bias = std::clamp(s_.bias, -1.0, 1.0);
    }

    inline void decide_insert(
        const SimulationState<N>& state,
        double cumulative_hazard,
        RNG* rng,
        std::vector<InsertDecision>& insert_decisions
    ) const {
        const auto ps = state.price_state();
        const auto ls = state.liq_state();

        if (!ps.best_bid || !ps.best_ask) {
            const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
            const Price_t ref = ps.last_trade_price;
            const Price_t px = (side == Side::BUY) ? (ref - 2) : (ref + 2);
            insert_decisions.push_back({ side, px, (Volume_t)1, Lifespan::GOOD_FOR_DAY, p_.hazard_max + cumulative_hazard });
            return;
        }

        const Price_t bb = *ps.best_bid;
        const Price_t ba = *ps.best_ask;
        const double spread_ticks = (double)(ba - bb);

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

        // --- Thinness at touch (depth0) to condition aggressiveness ---
        const double depth0 = (double)ls.bid_volumes[0] + (double)ls.ask_volumes[0];
        // crude normalization; ideally reuse your global thin01
        const double thin01 = std::clamp(1.0 - std::log1p(depth0) / 6.0, 0.0, 1.0);

        // --- Aggressiveness probabilities ---
        const double tight01 = std::clamp((2.0 - spread_ticks) / 2.0, 0.0, 1.0);

        // Cross more when urgent AND either tight OR thin (thin -> grab before it vanishes)
        double p_cross = p_.p_cross_base +
            (p_.p_cross_max - p_.p_cross_base) * urgency * (0.45 + 0.35 * tight01 + 0.20 * thin01);
        p_cross = std::clamp(p_cross, 0.0, 0.95);

        double p_improve = 0.08 * urgency * (1.0 - 0.7 * thin01);
        if (spread_ticks < 2.0) p_improve = 0.0;

        double p_join = std::max(p_.p_join_best_floor, 1.0 - p_cross - p_improve);

        const double Z = p_cross + p_improve + p_join;
        p_cross /= Z; p_improve /= Z; p_join /= Z;

        // --- Choose action and price ---
        const double u = rng->standard_uniform();
        Price_t px;

        if (u < p_cross) {
            // Depth-aware "sweep": choose how many levels to cross so expected fill >= qty
            const size_t levels = choose_sweep_levels_(side, qty, ls, urgency, rng);

            // Marketable limit price: last swept level (prevents absurd ba+10 jumps)
            // Assumes tick size = 1 and contiguous levels. If your book is sparse, map levels to price ladder.
            px = (side == Side::BUY) ? (Price_t)(ba + (Price_t)(levels ? (levels - 1) : 0))
                                    : (Price_t)(bb - (Price_t)(levels ? (levels - 1) : 0));
        } else if (u < p_cross + p_improve) {
            px = (side == Side::BUY) ? (Price_t)(bb + 1) : (Price_t)(ba - 1);
        } else {
            px = (side == Side::BUY) ? bb : ba;
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
        const LiquidityState<N>& ls,
        double urgency,
        RNG* rng
    ) const {
        // Try to sweep enough levels so that cumulative displayed volume >= qty
        // But allow randomness and urgency dependence (more urgent => more likely to sweep deeper).
        Volume_t cum = 0;
        size_t lvl = 0;

        const size_t max_lvl = N; // cap sweeps
        const double slack = 0.85 + 0.30 * urgency;             // urgent tolerates less shortfall

        while (lvl < max_lvl && (double)cum < slack * (double)qty) {
            const Volume_t v = (side == Side::BUY) ? ls.ask_volumes[lvl] : ls.bid_volumes[lvl];
            cum += v;
            ++lvl;
            // Random early stop: sometimes they don't fully sweep displayed depth
            if (lvl >= 2 && rng->standard_uniform() < (0.15 * (1.0 - urgency))) break;
        }

        return std::max<size_t>(1, lvl);
    }

};
