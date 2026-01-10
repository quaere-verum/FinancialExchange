#pragma once
#include "state.hpp"
#include "order_manager.hpp"
#include <optional>
#include <iostream>

enum struct EventType : uint8_t {
    INSERT_ORDER = 1,
    AMEND_ORDER  = 2,
    CANCEL_ORDER = 3
};

struct InsertDecision {
    Side side;
    Price_t price;
    Volume_t quantity;
    Lifespan lifespan;
};

struct CancelDecision {
    Id_t order_id;
};

struct AmendDecision {
    Id_t order_id;
    Volume_t new_quantity;
};

template<size_t N>
class MarketDynamics {
    public:
        InsertDecision decide_insert(const SimulationState<N>& state, RNG* rng) const {
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
            cross_prob = std::clamp(cross_prob, 0.0, 0.4);

            
            Price_t price;

            // ---------------------------------------------------------------------
            // 2. Determine price anchor
            // ---------------------------------------------------------------------
            Price_t anchor;

            if (side == Side::BUY) {
                if (ps.best_bid) {
                    anchor = std::max(1.0, std::round(*ps.best_bid * 0.7 + ls.fair_value * 0.3));
                } else {
                    anchor = std::max(1.0, std::round(ps.last_trade_price * 0.7 + ls.fair_value * 0.3));
                }
            } else {
                if (ps.best_ask) {
                    anchor = std::max(1.0, std::round(*ps.best_ask * 0.7 + ls.fair_value * 0.3));
                } else {
                    anchor = std::max(1.0, std::round(ps.last_trade_price * 0.7 + ls.fair_value * 0.3));
                }
            }

            if (rng->bernoulli(cross_prob)) {
                price = side == Side::BUY ? anchor + 1 : anchor - 1;
                price = std::clamp(price, MINIMUM_BID, MAXIMUM_ASK);
            } else {

                // ---------------------------------------------------------------------
                // 3. Aggressiveness / distance from anchor
                // ---------------------------------------------------------------------
                double spread_scale = 0.0;
                if (ps.spread) {
                    spread_scale = static_cast<double>(*ps.spread);
                }

                double vol_scale = vs.realised_vol_short() * static_cast<double>(anchor);

                double base_scale = spread_scale + 0.5 * vol_scale + 1e-8;

                // Heavy-tailed placement distance (typical in LOBs)
                double dist = rng->exponential(1.0 / base_scale);

                // Increase passivity under jump risk
                dist *= (1.0 + 2.0 * vs.jump_intensity);

                // ---------------------------------------------------------------------
                // 4. Compute limit price
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

            constexpr double S0 = 25.0;
            double base_scale = S0 + 0.3 * (fs.abs_volume_ewma - S0);

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

            return InsertDecision{
                side,
                price,
                qty,
                Lifespan::GOOD_FOR_DAY
            };
        }


        std::optional<CancelDecision> decide_cancel(const SimulationState<N>& state, const OrderManager& orders, RNG* rng) const {
            const auto& vs = state.vol_state();
            const FlowState& fs = state.flow_state();
            const LiquidityState<N>& liq = state.liq_state();

            double side_score = 0.0;
            side_score += 0.7 * fs.flow_imbalance;
            if (liq.has_bid_side && liq.has_ask_side) {
                side_score += 0.5 * liq.imbalances[0];
            }

            double buy_prob = 0.5 + 0.25 * std::tanh(side_score);
            buy_prob = std::clamp(buy_prob, 0.05, 0.95);

            Side side = rng->bernoulli(buy_prob) ? Side::BUY : Side::SELL;
            auto order_opt = orders.random_order(rng, side);
            if (!order_opt) return std::nullopt;

            const OrderInfo& order = *order_opt;

            double age_score  = orders.order_age(order, state.time_state().sim_time) / 10.0;
            double dist_score = orders.distance_from_touch(order, state.price_state()) / 10.0;
            double depth_score = orders.depth_ahead(order) / 100.0;

            double cancel_score = 0.4 * age_score + 0.3 * dist_score + 0.2 * depth_score + 0.1 * vs.realised_vol_short();
            cancel_score = std::clamp(cancel_score, 0.0, 1.0);

            if (!rng->bernoulli(cancel_score)) return std::nullopt;
            return CancelDecision{ order.order_id };
        }


        std::optional<AmendDecision> decide_amend(
            const SimulationState<N>& state,
            const OrderManager& orders,
            RNG* rng
        ) const {
            const FlowState& fs = state.flow_state();
            const LiquidityState<N>& liq = state.liq_state();

            double side_score = 0.0;
            side_score += 0.7 * fs.flow_imbalance;
            if (liq.has_bid_side && liq.has_ask_side) {
                side_score += 0.5 * liq.imbalances[0];
            }

            double buy_prob = 0.5 + 0.25 * std::tanh(side_score);
            buy_prob = std::clamp(buy_prob, 0.05, 0.95);
            Side side = rng->bernoulli(buy_prob) ? Side::BUY : Side::SELL;
            auto order_opt = orders.random_order(rng, side);
            if (!order_opt) return std::nullopt;

            const OrderInfo& order = *order_opt;
            const VolatilityState& vs = state.vol_state();

            double age_score = orders.order_age(order, state.time_state().sim_time) / 10.0;
            double dist_score = orders.distance_from_touch(order, state.price_state()) / 10.0;
            double depth_score = orders.depth_ahead(order) / 100.0;
            double vol_score = vs.realised_vol_short();

            double amend_score = 0.4 * age_score + 0.3 * dist_score + 0.2 * depth_score + 0.1 * vol_score;
            amend_score = std::clamp(amend_score, 0.0, 1.0);

            amend_score *= (0.8 + 0.4 * rng->standard_uniform());
            amend_score = std::clamp(amend_score, 0.0, 1.0);

            if (!rng->bernoulli(amend_score)) return std::nullopt;

            double size_scale = static_cast<double>(order.quantity);

            double log_qty = std::log(size_scale) + 0.3 * rng->standard_normal();  // moderate perturbation
            Volume_t new_qty = static_cast<Volume_t>(std::max(1.0, std::exp(log_qty)));
            if (new_qty >= order.quantity) return std::nullopt;

            return AmendDecision{
                order.order_id,
                new_qty
            };
        }

        
        void update_intensities(
            const SimulationState<N>& state,
            double& lambda_insert,
            double& lambda_amend,
            double& lambda_cancel
        ) {
            constexpr double base_insert_rate  = 2000.0; // per second
            constexpr double base_cancel_rate  = 4000.0;
            constexpr double base_amend_rate   = 1500.0;

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

            lambda_insert = base_insert_rate * insert_multiplier;
           
            double cancel_multiplier =
                1.0
                + 1.2 * vol.realised_vol_short()
                + 0.8 * std::abs(fs.flow_imbalance)
                + 0.6 * std::min(fs.abs_volume_ewma / 100.0, 2.0)
                + 0.5 / std::max(depth_near_touch, 1.0);

            lambda_cancel = base_cancel_rate * cancel_multiplier;

            double amend_multiplier =
                1.0
                + 0.9 * vol.realised_vol_short()
                + 0.6 * std::abs(fs.flow_imbalance)
                + 0.4 / std::max(depth_near_touch, 1.0);

            lambda_amend = base_amend_rate * amend_multiplier;
        }


};
