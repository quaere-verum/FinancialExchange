#pragma once
#include "types.hpp"
#include "util.hpp"
#include "rng.hpp"
#include "state.hpp"
#include <vector>
#include "parameters.hpp"

static inline void decide_noise_insert(
    const FeatureVector& state,
    double cumulative_hazard,
    RNG* rng,
    const NoiseParams& p,
    std::vector<InsertDecision>& insert_decisions
) {
    // If no BBO, place around last trade with small offset
    if (!state.has_ask_side || !state.has_bid_side) {
        const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
        const Price_t ref = state.last_trade_price;
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

    const double p_buy = std::clamp(0.5 + 0.10 * p.flow_follow * state.flow_imbalance, 0.40, 0.60);
    const Side side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

    // Marketable prob increases when spread is tight (people hit/lift more when cheap)
    double p_mkt = p.p_marketable_base + (p.p_marketable_max - p.p_marketable_base) * state.tight01;
    p_mkt = std::clamp(p_mkt, 0.0, 0.5);

    // Choose price
    Price_t px;
    if (rng->standard_uniform() < p_mkt) {
        // marketable with small chance: place at opposite best (best-only take)
        px = (side == Side::BUY) ? *state.best_ask : *state.best_bid;
    } else {
        // non-marketable: place on own side at some offset from best
        const int k = sample_geometric_offset(rng, 0.75, p.max_offset_ticks);
        px = (side == Side::BUY) ? (*state.best_bid - (Price_t)(k))
                                 : (*state.best_ask + (Price_t)(k));
    }

    const double z = rng->standard_normal();
    const double base = std::max(1e-6, p.mean_qty);
    double q = base * std::exp(p.qty_sigma * z);
    q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);

    insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass + cumulative_hazard });
    return;
}