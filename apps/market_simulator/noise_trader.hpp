#pragma once
#include "types.hpp"
#include "util.hpp"
#include "rng.hpp"
#include "state.hpp"
#include <vector>


struct NoiseParams {
    // Noise places mostly near-touch but often non-marketable.
    // Offsets are in ticks from best on that side.
    int max_offset_ticks = 6;

    // Probability of being marketable (small, increases when spread is tight)
    double p_marketable_base = 0.01;
    double p_marketable_max  = 0.10;

    // Sizes (small)
    Volume_t min_qty = 1;
    Volume_t max_qty = 30;
    double mean_qty = 3.0;
    double qty_sigma = 0.7;

    // Noise cancels quickly / is flaky
    double hazard_mass = 0.9;
    double lifespan = 0.15;

    // Side is near-uniform; can add tiny drift from flow
    double flow_follow = 0.10; // 0..1, small
};

template <size_t N>
static inline void decide_noise_insert(
    const SimulationState<N>& state,
    double cumulative_hazard,
    RNG* rng,
    const NoiseParams& p,
    std::vector<InsertDecision>& insert_decisions
) {
    const auto ps = state.price_state();
    const auto vs = state.vol_state();
    const auto fs = state.flow_state();

    // If no BBO, place around last trade with small offset
    if (!ps.best_bid || !ps.best_ask) {
        const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
        const Price_t ref = ps.last_trade_price;
        const int k = 1 + sample_geometric_offset(rng, 0.80, p.max_offset_ticks);
        const Price_t px = (side == Side::BUY) ? (ref - (Price_t)(k))
                                               : (ref + (Price_t)(k));
        const double z = rng->standard_normal();
        const double base = std::max(1e-6, p.mean_qty);
        double q = base * std::exp(p.qty_sigma * z);
        q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);
        insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass + cumulative_hazard });
        return;
    }

    const Price_t bb = *ps.best_bid;
    const Price_t ba = *ps.best_ask;
    const double spread_ticks = (double)(ba - bb);

    // Side: almost fair coin, tiny flow-following drift
    const double flow = fs.flow_imbalance;
    const double p_buy = std::clamp(0.5 + 0.10 * p.flow_follow * flow, 0.40, 0.60);
    const Side side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

    // Marketable prob increases when spread is tight (people hit/lift more when cheap)
    const double tight01 = std::exp(-0.4605 * spread_ticks);
    double p_mkt = p.p_marketable_base + (p.p_marketable_max - p.p_marketable_base) * tight01;
    p_mkt = std::clamp(p_mkt, 0.0, 0.5);

    // Choose price
    Price_t px;
    if (rng->standard_uniform() < p_mkt) {
        // marketable with small chance: place at opposite best (best-only take)
        px = (side == Side::BUY) ? ba : bb;
    } else {
        // non-marketable: place on own side at some offset from best
        const int k = sample_geometric_offset(rng, 0.75, p.max_offset_ticks);
        px = (side == Side::BUY) ? (bb - (Price_t)(k))
                                 : (ba + (Price_t)(k));
    }

    const double z = rng->standard_normal();
    const double base = std::max(1e-6, p.mean_qty);
    double q = base * std::exp(p.qty_sigma * z);
    q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);

    insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass + cumulative_hazard });
    return;
}