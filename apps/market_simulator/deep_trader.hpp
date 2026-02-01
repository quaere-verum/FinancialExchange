#pragma once
#include "types.hpp"
#include "util.hpp"
#include "state.hpp"
#include "rng.hpp"

#include <vector>

struct DeepParams {
    // Deep placement region (ticks from best)
    int min_offset_ticks = 5;    // do not clutter near-touch
    int max_offset_ticks = 40;

    // Tail: higher stress => smaller p => deeper tail (more far quotes)
    double p_base = 0.75;        // geometric p when calm
    double p_stress_slope = 0.35;

    // Sizes
    Volume_t min_qty = 1;
    Volume_t max_qty = 800;
    double mean_qty = 20.0;
    double qty_sigma = 1.0;

    // Cancellation behavior: deep liquidity is “sticky”
    double hazard_mass = 0.15;   // low hazard mass => long-lived]

    // Mild contrarian to flow (deep providers refill the pressured side)
    double flow_contrarian = 0.25; // 0..1
};

template <size_t N>
static inline void decide_deep_insert(
    const SimulationState<N>& state,
    double cumulative_hazard,
    RNG* rng,
    const DeepParams& p,
    std::vector<InsertDecision>& insert_decisions
) {
    const auto ps = state.price_state();
    const auto ls = state.liq_state();
    const auto vs = state.vol_state();
    const auto fs = state.flow_state();
    const auto lat = state.latent_state();

    // If we have no BBO, place around last trade / fair
    if (!ps.best_bid || !ps.best_ask) {
        const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
        const Price_t ref = ps.last_trade_price;
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

    const Price_t bb = *ps.best_bid;
    const Price_t ba = *ps.best_ask;

    // Stress proxy: log(vol_short/vol_long), positive part
    const double vol_s = vs.realised_vol_short;
    const double vol_l = vs.realised_vol_long;
    const double stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);
    const double stress01 = std::clamp(std::max(0.0, stress) / 1.5, 0.0, 1.0);

    // Side: mostly contrarian to flow; optionally incorporate top imbalance
    const double flow = fs.flow_imbalance;
    const double imb0 = ls.imbalances[0];

    // Deep providers often add liquidity to the pressured side: if flow is BUY, they place SELL (ask) liquidity.
    double score = -p.flow_contrarian * flow - 0.10 * imb0;

    // Very weak fair anchor: if fair above mid, slightly more buy-side depth
    const double mid = 0.5 * ((double)bb + (double)ba);
    const double fair_gap = std::clamp((lat.fair_value - mid) / std::max(1.0, mid), -0.02, 0.02) / 0.02;
    score += 0.05 * fair_gap;

    const double p_buy = std::clamp(0.5 + 0.30 * std::tanh(score), 0.05, 0.95);
    const Side side = (rng->standard_uniform() < p_buy) ? Side::BUY : Side::SELL;

    // Offset: geometric tail with stress => fatter tail (place deeper)
    double p_geom = p.p_base - p.p_stress_slope * stress01; // lower p => heavier tail
    p_geom = std::clamp(p_geom, 0.20, 0.90);

    int k = p.min_offset_ticks + sample_geometric_offset(rng, p_geom, p.max_offset_ticks - p.min_offset_ticks);
    k = std::clamp(k, p.min_offset_ticks, p.max_offset_ticks);

    const Price_t px = (side == Side::BUY) ? (bb - (Price_t)(k))
                                           : (ba + (Price_t)(k));

    const double z = rng->standard_normal();
    const double base = std::max(1e-6, p.mean_qty);
    double q = base * std::exp(p.qty_sigma * z);
    q = std::clamp(q, (double)p.min_qty, (double)p.max_qty);

    return insert_decisions.push_back({ side, px, (Volume_t)std::llround(q), Lifespan::GOOD_FOR_DAY, p.hazard_mass + cumulative_hazard});
}