#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "types.hpp"
#include "util.hpp"
#include "rng.hpp"
#include "state.hpp"

struct MarketMakerParams {
    // --- Quoting style ---
    double p_improve_when_wide = 0.65;     // if spread >= 2 ticks, improve inside with this prob
    double p_one_sided_when_stressed = 0.15; // probability to quote only one side when stressed

    // --- Size model ---
    Volume_t min_qty = 1;
    Volume_t max_qty = 50;
    double mean_qty = 5.0;                // mean of qty distribution (approx)
    double qty_logn_sigma = 0.6;          // tail heaviness for lognormal

    // --- Stress response ---
    double stress_spread_strength = 0.7;  // widen => more conservative
    double stress_vol_strength = 0.9;     // higher short/long vol => more conservative
    double stress_flow_strength = 0.6;    // high |flow| => more conservative

    double base_hazard_mass = 1.0;        // baseline hazard mass per order
    double hazard_mass_min = 0.2;
    double hazard_mass_max = 4.0;

    // Hazard mass scaling (bigger => more likely to cancel sooner if hazard accumulator is uniform)
    double hazard_stress_strength = 1.2;  // stressed => higher hazard mass
};

struct MarketMakerLatentState {
    double risk = 0.0;
    double bias = 0.0;
    double log_depth0_ref = 0.0;
    bool depth_ref_init = false;
};

template <size_t N>
class MarketMakerAgent {
    public:
        explicit MarketMakerAgent(MarketMakerParams p = {}) : p_(p) {}

        MarketMakerLatentState& state() { return s_; }
        const MarketMakerLatentState& state() const { return s_; }

        template<size_t N>
        void update(
            const SimulationState<N>& state
        ) {
            const auto ts = state.time_state();
            const auto ps = state.price_state();
            const auto ls = state.liq_state();
            const auto vs = state.vol_state();
            const auto fs = state.flow_state();
            const auto lat = state.latent_state();

            const double dt = std::max(0.0, ts.time_since_previous_sync);
            if (dt <= 0.0) return;

            const double spread = ps.spread ? (double)(*ps.spread) : 0.0;
            const double spread01 = std::clamp(spread / 2.0, 0.0, 2.0) / 2.0; // 0..1 when spread<=2 ticks

            const double vol_s = vs.realised_vol_short;
            const double vol_l = vs.realised_vol_long;
            const double vol_stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);
            const double vol01 = std::clamp(std::max(0.0, vol_stress) / 1.5, 0.0, 1.0);

            const double flow = fs.flow_imbalance;
            const double absflow = std::abs(flow);

            const double surp01 = std::clamp(fs.volume_surprise, 0.0, 2.0) / 2.0;

            const double imb0 = std::clamp(ls.imbalances[0], -1.0, 1.0);

            const double depth0 = (double)ls.bid_volumes[0] + (double)ls.ask_volumes[0];
            const double logd0 = std::log(depth0 + 1.0);
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
            const double thin01 = std::clamp(s_.log_depth0_ref - logd0, 0.0, 2.0) / 2.0;

            // --- RISK update (EWMA to target) ---
            constexpr double TAU_RISK = 0.5; // seconds (fast-ish)
            const double a_r = 1.0 - std::exp(-dt / TAU_RISK);

            const double r_target = std::clamp(
                0.35 * spread01 +
                0.40 * vol01 +
                0.35 * absflow +
                0.20 * surp01 +
                0.30 * thin01,
                0.0, 1.0
            );

            s_.risk = (1.0 - a_r) * s_.risk + a_r * r_target;
            s_.risk = std::clamp(s_.risk, 0.0, 1.0);

            // --- BIAS update (OU to target) ---
            constexpr double TAU_BIAS = 1.2; // seconds (slower)
            const double a_b = 1.0 - std::exp(-dt / TAU_BIAS);

            // fair gap (weak)
            const double mid = (ps.best_bid && ps.best_ask) ? 0.5 * ((double)*ps.best_bid + (double)*ps.best_ask)
                                                            : (double)ps.last_trade_price;
            const double fair_gap = std::clamp((lat.fair_value - mid) / std::max(1.0, mid), -0.02, 0.02) / 0.02; // now in [-1,1]

            const double b_target = std::clamp(
                0.70 * (-flow) +      // contrarian to taker pressure
                0.25 * (-imb0) +      // lean against local book imbalance
                0.15 * (fair_gap),    // weak anchor
                -1.0, 1.0
            );

            s_.bias = (1.0 - a_b) * s_.bias + a_b * b_target;

            // Damp leaning when risk is high (makers stop “positioning” and reduce exposure)
            s_.bias *= (1.0 - 0.6 * s_.risk);
            s_.bias = std::clamp(s_.bias, -1.0, 1.0);
        }

        InsertDecision decide_insert(const SimulationState<N>& state, double cumulative_hazard, RNG* rng) {
            const auto ps = state.price_state();
            const auto ls = state.liq_state();
            const auto vs = state.vol_state();
            const auto fs = state.flow_state();
            const auto lat = state.latent_state();
            const auto ts = state.time_state();

            // If we don't have both sides, behave conservatively: provide on existing side only
            const bool has_bbo = ps.best_bid.has_value() && ps.best_ask.has_value();
            if (!has_bbo) {
                // If only one side exists, quote opposite side at some distance from last trade.
                return decide_when_book_incomplete_(state, cumulative_hazard, rng);
            }

            const Price_t bb = *ps.best_bid;
            const Price_t ba = *ps.best_ask;
            const double spread = static_cast<double>(ba - bb);

            // Basic depth / imbalance near touch (bucket 0)
            const double vb0 = static_cast<double>(ls.bid_volumes[0]);
            const double va0 = static_cast<double>(ls.ask_volumes[0]);
            const double imb0 = ls.imbalances[0];
            const double depth0 = vb0 + va0;

            // Vol stress proxy: log(vol_short / vol_long)
            const double vol_s = vs.realised_vol_short;
            const double vol_l = vs.realised_vol_long;
            const double vol_stress = std::clamp(std::log((vol_s + 1e-8) / (vol_l + 1e-8)), -2.0, 2.0);
            const double vol_stress_p = std::max(0.0, vol_stress);

            // Flow pressure: executed imbalance (taker-driven). For MM, often contrarian.
            const double flow = fs.flow_imbalance;
            const double absflow = std::abs(flow);

            // Fair vs mid signal (weak anchor)
            const double mid = (double)(bb + ba) * 0.5;
            const double fair = lat.fair_value;
            const double fair_gap = std::clamp((fair - mid) / std::max(1.0, mid), -0.02, 0.02); // normalized

            // Combined stress scalar (dimensionless)
            const double stress =
                p_.stress_spread_strength * std::clamp(spread / 2.0, 0.0, 4.0) +
                p_.stress_vol_strength    * vol_stress_p +
                p_.stress_flow_strength   * absflow;

            // Decide whether we quote one side only (during stress or pullback)
            const bool one_sided = rng->standard_uniform() < (p_.p_one_sided_when_stressed * std::clamp(stress / 2.0, 0.0, 1.0));

            // Choose side with skew from inventory + flow + fair_gap + local imbalance
            const Side side = choose_side_(imb0, flow, fair_gap, rng);

            // Choose price: join best or improve inside if spread wide enough
            const Price_t px = choose_price_(side, bb, ba, spread, depth0, rng);

            // Choose size
            const Volume_t qty = sample_qty_(rng);

            // Lifespan & hazard mass (primary control is hazard mass)
            Lifespan lifespan = Lifespan::GOOD_FOR_DAY;
            const double hazard_mass = choose_hazard_mass_(depth0);

            if (one_sided) {
                // Return as decided (caller will place single insert)
                return { side, px, qty, lifespan, hazard_mass + cumulative_hazard };
            }

            // Two-sided quoting: you’ll likely call decide_insert() once per insert event.
            // So we keep this method returning a *single* InsertDecision. To emulate two-sided behavior,
            // we alternate sides on successive calls unless inventory skew strongly forces one side.
            return { side, px, qty, lifespan, hazard_mass + cumulative_hazard };
        }

    private:
        MarketMakerParams p_;
        MarketMakerLatentState s_;

        inline InsertDecision decide_when_book_incomplete_(const SimulationState<N>& state, double cumulative_hazard, RNG* rng) {
            const auto ps = state.price_state();
            // If one side missing, be conservative: place away from last_trade_price.
            const Price_t ref = ps.last_trade_price;
            const Price_t d = 4 + sample_geometric_offset(rng, 0.33, 6);

            if (!ps.best_bid && ps.best_ask) {
                // Provide bid
                const Price_t px = *ps.best_ask - d;
                return { Side::BUY, px, (Volume_t)1, Lifespan::GOOD_FOR_DAY, p_.base_hazard_mass + cumulative_hazard};
            }
            if (ps.best_bid && !ps.best_ask) {
                // Provide ask
                const Price_t px = *ps.best_bid + d;
                return { Side::SELL, px, (Volume_t)1, Lifespan::GOOD_FOR_DAY, p_.base_hazard_mass + cumulative_hazard };
            }
            // No book: quote around last trade
            const Side side = (rng->standard_uniform() < 0.5) ? Side::BUY : Side::SELL;
            const Price_t px = (side == Side::BUY) ? (ref - d) : (ref + d);
            return { side, px, (Volume_t)1, Lifespan::GOOD_FOR_DAY, p_.base_hazard_mass + cumulative_hazard};
        }

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

            // optionally: improve less when flow is extreme; but do this in risk update, not here
            p_improve = std::clamp(p_improve, 0.01, 0.90);

            if (can_improve && rng->standard_uniform() < p_improve) {
                return (side == Side::BUY) ? (bb + 1) : (ba - 1);
            }

            return (side == Side::BUY) ? bb : ba;
        }

        inline Volume_t sample_qty_(RNG* rng) const {

            const double z = rng->standard_normal();
            const double base = std::max(1e-6, p_.mean_qty);
            double q = base * std::exp(p_.qty_logn_sigma * z);
            q *= (1.0 - 0.35 * s_.risk);

            q = std::clamp(q, (double)p_.min_qty, (double)p_.max_qty);
            return (Volume_t)std::llround(q);
        }

        inline double choose_hazard_mass_(double depth0) const {
            const double logd0 = std::log(depth0 + 1.0);
            const double thin_raw = std::clamp(s_.log_depth0_ref - logd0, 0.0, 2.0);
            const double thin01 = thin_raw / 2.0;

            double m = p_.base_hazard_mass;

            m *= (1.0 + p_.hazard_stress_strength * s_.risk);
            m *= (1.0 + 0.6 * thin01);

            return std::clamp(m, p_.hazard_mass_min, p_.hazard_mass_max);
        }

};
