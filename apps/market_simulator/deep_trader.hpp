#pragma once
#include "types.hpp"
#include "util.hpp"
#include "state.hpp"
#include "rng.hpp"
#include "parameters.hpp"

#include <vector>


static inline void decide_deep_insert(
    const FeatureVector& state,
    double cumulative_hazard,
    RNG* rng,
    const DeepParams& p,
    std::vector<InsertDecision>& insert_decisions
) {
    // If we have no BBO, place around last trade / fair
    if (!state.has_bid_side || !state.has_ask_side) {
        const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
        const Price_t ref = state.last_trade_price;
        const int k = p.min_offset_ticks + sample_geometric_offset(rng, 0.65, p.max_offset_ticks - p.min_offset_ticks);
        const Price_t px = (side == Side::BUY) ? (ref - (Price_t)(k))
                                               : (ref + (Price_t)(k));
        const double z = rng->standard_normal();
        const double base = std::max(1e-6, p.mean_qty);
        double q = base * std::exp(p.qty_sigma * z);
        q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);
        insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass });
        return;
    }

    // Deep providers often add liquidity to the pressured side: if flow is BUY, they place SELL (ask) liquidity.
    double score = -p.flow_contrarian * state.flow_imbalance - 0.10 * state.imbalance_at_touch;

    // Very weak fair anchor: if fair above mid, slightly more buy-side depth
    const double mid = 0.5 * ((double)*state.best_bid + (double)*state.best_ask);
    const double fair_gap = std::clamp((state.fair_value - mid) / std::max(1.0, mid), -0.02, 0.02) / 0.02;
    score += 0.05 * fair_gap;

    const double p_buy = std::clamp(0.5 + 0.30 * std::tanh(score), 0.05, 0.95);
    const Side side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

    // Offset: geometric tail with stress => fatter tail (place deeper)
    double p_geom = p.p_base - p.p_stress_slope * state.stress01; // lower p => heavier tail
    p_geom = std::clamp(p_geom, 0.20, 0.90);

    int k = p.min_offset_ticks + sample_geometric_offset(rng, p_geom, p.max_offset_ticks - p.min_offset_ticks);
    k = std::clamp(k, p.min_offset_ticks, p.max_offset_ticks);

    const Price_t px = (side == Side::BUY) ? (*state.best_bid - (Price_t)(k))
                                           : (*state.best_ask + (Price_t)(k));

    const double z = rng->standard_normal();
    const double base = std::max(1e-6, p.mean_qty);
    double q = base * std::exp(p.qty_sigma * z);
    q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);

    return insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass + cumulative_hazard});
}