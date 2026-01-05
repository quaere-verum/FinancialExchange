#pragma once
#include "state.hpp"
#include "order_manager.hpp"
#include <optional>

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
            const auto& ps = state.price_state();
            const auto& fs = state.flow_state();
            const auto& vs = state.vol_state();

            const double flow_bias = fs.signed_volume_ewma / (fs.traded_volume_ewma + 1e-8);

            Side side = flow_bias > 0.0 ? Side::SELL : Side::BUY;

            const double vol = vs.realised_vol_short();
            const double spread = static_cast<double>(ps.spread);

            const double offset = std::max(spread, vol * ps.mid_price * 0.5);

            Price_t price = std::min(std::max(
                side == Side::BUY ? ps.mid_price - static_cast<Price_t>(offset) : ps.mid_price + static_cast<Price_t>(offset),
                MINIMUM_BID
            ), MAXIMUM_ASK);

            Volume_t qty = static_cast<Volume_t>(std::max(1.0, fs.traded_volume_ewma * 0.1));

            return {side, price, qty, Lifespan::GOOD_FOR_DAY};
        }

        std::optional<CancelDecision> decide_cancel(const SimulationState<N>& state, const OrderManager& orders, RNG* rng) const {
            const auto& vs = state.vol_state();
            const auto& order_opt = orders.random_order(rng, Side::BUY);

            if (!order_opt) {
                return std::nullopt;
            }

            return CancelDecision{order_opt->order_id};
        }

        std::optional<AmendDecision> decide_amend(const SimulationState<N>& state, const OrderManager& orders, RNG* rng) const {
            const auto& ps = state.price_state();
            const auto& vs = state.vol_state();

            const auto& order_opt = orders.random_order(rng, Side::BUY);

            if (!order_opt) {
                return std::nullopt;
            }

            return AmendDecision{order_opt->order_id, order_opt->quantity};
        }
};
