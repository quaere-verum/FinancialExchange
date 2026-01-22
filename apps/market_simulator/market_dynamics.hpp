#pragma once
#include "state.hpp"
#include "order_manager.hpp"
#include <optional>
#include <iostream>
#include <vector>

constexpr double LAMBDA_INSERT_BASE = 5'000.0;
constexpr double CANCEL_SCALING_FACTOR = 10'000.0;
constexpr double BASE_ORDER_SIZE = 25.0;
constexpr double LAMBDA_CANCEL_BASE = LAMBDA_INSERT_BASE / CANCEL_SCALING_FACTOR;


struct InsertDecision {
    Side side;
    Price_t price;
    Volume_t quantity;
    Lifespan lifespan;
    double cancellation_hazard_mass;
};

template<size_t N>
class MarketDynamics {
    public:
        InsertDecision decide_insert(const SimulationState<N>& state, double cumulative_hazard, RNG* rng) const {
            const PriceState& ps  = state.price_state();
            const VolatilityState& vs  = state.vol_state();
            const FlowState& fs  = state.flow_state();
            const LiquidityState<N>& liq = state.liq_state();
            const LatentState& ls = state.latent_state();

            // ---------------------------------------------------------------------
            // 1. Choose side (slightly biased by flow imbalance if available)
            // ---------------------------------------------------------------------
            double side_score = 0.0;

            // Signed flow: positive → buy pressure, negative → sell pressure
            side_score += 0.7 * fs.flow_imbalance;
;

            // Near-touch book imbalance (bucket 0)
            if (liq.has_bid_side && liq.has_ask_side) {
                side_score += 0.5 * liq.imbalances[0];
            }

            // Convert to probability
            double buy_prob = 0.5 + 0.25 * std::tanh(side_score);
            buy_prob = std::clamp(buy_prob, 0.05, 0.95);

            Side side = rng->bernoulli(buy_prob) ? Side::BUY : Side::SELL;

            double cross_prob = 0.0;

            if (ps.spread) {
                cross_prob += 0.1 / (1.0 + static_cast<double>(*ps.spread));
            }

            cross_prob += 0.2 * std::abs(fs.flow_imbalance);
            cross_prob += 0.2 * vs.realised_vol_short();
            cross_prob = std::clamp(cross_prob, 0.1, 0.7);

            
            Price_t price;

            // ---------------------------------------------------------------------
            // 2. Determine price anchor
            // ---------------------------------------------------------------------
            Price_t anchor;
            constexpr double best_price_weight = 0.6;
            constexpr double fair_value_weight = 0.4;

            if (side == Side::BUY) {
                if (ps.best_bid) {
                    anchor = std::max(1.0, std::round(*ps.best_bid * best_price_weight + ls.fair_value * fair_value_weight));
                } else {
                    anchor = std::max(1.0, std::round(ps.last_trade_price * best_price_weight + ls.fair_value * fair_value_weight));
                }
            } else {
                if (ps.best_ask) {
                    anchor = std::max(1.0, std::round(*ps.best_ask * best_price_weight + ls.fair_value * fair_value_weight));
                } else {
                    anchor = std::max(1.0, std::round(ps.last_trade_price * best_price_weight + ls.fair_value * fair_value_weight));
                }
            }

            double dist = 0.0;
            if (rng->bernoulli(cross_prob)) {
                dist = rng->categorical(crossing_ticks_probs_);
                price = side == Side::BUY ? anchor + static_cast<Price_t>(dist) : anchor - static_cast<Price_t>(dist);
                price = std::clamp(price, MINIMUM_BID, MAXIMUM_ASK);
            } else {

                // ---------------------------------------------------------------------
                // 3. Aggressiveness / distance from anchor
                // ---------------------------------------------------------------------
                double spread_scale = 0.0;
                if (ps.spread) {
                    spread_scale = static_cast<double>(*ps.spread);
                }

                auto vol_long = vs.realised_vol_long();
                double vol_regime;
                if (vol_long > 0.0) {
                    vol_regime = vs.realised_vol_short() / vol_long;
                } else {
                    vol_regime = 0.0;
                }
                double vol_scale = std::clamp(std::pow(vol_regime, 2.0), 1.0, 25.0);
                double base_scale = 
                    1.0
                    + 0.6 * spread_scale 
                    + 0.4 * vol_scale;

                // Heavy-tailed placement distance (typical in LOBs)
                dist = rng->exponential(1.0 / base_scale);

                // Increase passivity under jump risk
                dist *= (1.0 + 2.0 * vs.jump_intensity);

                // ---------------------------------------------------------------------
                // 4. Compute limit  
                // ---------------------------------------------------------------------
                if (side == Side::BUY) {
                    price = static_cast<Price_t>(anchor - dist);
                    price = std::max(price, MINIMUM_BID);
                } else {
                    price = static_cast<Price_t>(anchor + dist);
                    price = std::min(price, MAXIMUM_ASK);
                }
            }
            
            // ---------------------------------------------------------------------
            // 5. Size determination
            // ---------------------------------------------------------------------
            double aggressiveness = 0.5;  // neutral default

            if (ps.best_bid && ps.best_ask && ps.spread && *ps.spread > 0) {
                if (side == Side::BUY) {
                    if (price >= *ps.best_bid) {
                        aggressiveness = 1.0;
                    } else {
                        aggressiveness =
                            std::exp(
                                -(static_cast<double>(*ps.best_bid - price))
                                / (static_cast<double>(*ps.spread) + 1e-8)
                            );
                    }
                } else {
                    if (price <= *ps.best_ask) {
                        aggressiveness = 1.0;
                    } else {
                        aggressiveness =
                            std::exp(
                                -(static_cast<double>(price - *ps.best_ask))
                                / (static_cast<double>(*ps.spread) + 1e-8)
                            );
                    }
                }
            } else {
                // One-sided or undefined book → treat as urgent but uncertain
                aggressiveness = 0.7;
            }

            aggressiveness = std::clamp(aggressiveness, 0.1, 1.0);

            double near_depth = 0.0;

            if (side == Side::BUY && liq.has_bid_side) {
                near_depth = static_cast<double>(liq.bid_volumes[0]);
            } else if (side == Side::SELL && liq.has_ask_side) {
                near_depth = static_cast<double>(liq.ask_volumes[0]);
            }

            double base_scale = BASE_ORDER_SIZE + 0.3 * (fs.abs_volume_ewma - BASE_ORDER_SIZE);

            // Depth effect (concave, capped)
            double depth_factor = std::sqrt(std::min(near_depth, 100.0) + 1.0);

            // Imbalance effect: smaller if strong flow pressure
            double imbalance_factor = 1.0 - 0.5 * std::abs(fs.flow_imbalance);

            // Aggressiveness effect
            double aggressiveness_factor = 0.3 + 0.7 * (1.0 - aggressiveness);

            // Volume surprise (moderate)
            double surprise_factor = std::clamp(1.0 + fs.volume_surprise, 0.8, 1.5);

            // Final scale
            double size_scale = base_scale
                            * depth_factor
                            * imbalance_factor
                            * aggressiveness_factor
                            * surprise_factor;
            // Safety floor
            size_scale = std::max(size_scale, 1.0);

            // Lognormal dispersion (empirical)
            double log_qty = std::log(size_scale) + 0.6 * rng->standard_normal();

            Volume_t qty = static_cast<Volume_t>(std::max(1.0, std::exp(log_qty)));

            double distance_multiplier = std::exp(0.025 * dist);

            double u = rng->standard_uniform();
            double hazard_mass = -std::log(u) / distance_multiplier;

            return InsertDecision{
                side,
                price,
                qty,
                Lifespan::GOOD_FOR_DAY,
                cumulative_hazard + hazard_mass
            };
        }


        
        void update_intensity(
            const SimulationState<N>& state,
            size_t open_order_count,
            double& lambda_insert,
            double& lambda_cancel
        ) {

            const auto& ps = state.price_state();
            const auto& fs = state.flow_state();
            const auto& liq = state.liq_state();
            const auto& vol = state.vol_state();

            double depth_near_touch = 1.0;
            if (liq.has_bid_side && liq.has_ask_side) {
                depth_near_touch = static_cast<double>(liq.bid_volumes[0] + liq.ask_volumes[0]);
            }

            double insert_multiplier =
                1.0
                + 0.6 * std::abs(fs.flow_imbalance)
                + 0.8 * vol.realised_vol_short()
                + 0.4 / std::max(depth_near_touch, 1.0);

            lambda_insert = LAMBDA_INSERT_BASE * insert_multiplier;

            double depth_multiplier = 0.5 + static_cast<double>(open_order_count) / CANCEL_SCALING_FACTOR;
            double vol_multiplier = 1.0 + 1.5 * std::min(vol.realised_vol_short(), 1.0);
            double flow_multiplier = 1.0 + 0.8 * std::abs(fs.flow_imbalance);
            flow_multiplier = std::max(flow_multiplier, 0.3);
            double spread_multiplier = 1.0;
            if (ps.spread) {
                spread_multiplier = 1.0 + 0.3 * static_cast<double>(*ps.spread);
            }

            lambda_cancel =
                LAMBDA_CANCEL_BASE
                * depth_multiplier
                * vol_multiplier
                * flow_multiplier
                * spread_multiplier;

        }

    private:
        std::vector<double> crossing_ticks_probs_ = {
            0.60,  // touch
            0.60 + 0.25,  // 1 tick
            0.60 + 0.25 + 0.10,  // 2 ticks
            0.60 + 0.25 + 0.10 + 0.04,  // 3 ticks
            0.60 + 0.25 + 0.10 + 0.04 + 0.01   // 4 ticks
        };

};
