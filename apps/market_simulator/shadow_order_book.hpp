#pragma once
#include "protocol.hpp"
#include "types.hpp"
#include <map>

class ShadowOrderBook {
    public:
        void on_order_book_snapshot(const PayloadOrderBookSnapshot* snapshot) {
            bids_.clear();
            asks_.clear();
            for (size_t idx = 0; idx < snapshot->ask_prices.size(); ++idx) {
                asks_[snapshot->ask_prices[idx]] = snapshot->ask_volumes[idx];
                bids_[snapshot->bid_prices[idx]] = snapshot->bid_volumes[idx];
            }
        }

        void on_price_level_update(const PayloadPriceLevelUpdate* update) {
            auto& levels = update->side == Side::BUY ? bids_ : asks_;
            if (update->total_volume == 0) {
                levels.erase(update->price);
            } else {
                levels[update->price] = update->total_volume;
            }
        }

        Price_t best_bid_price() const { return bids_.empty() ? MINIMUM_BID : bids_.rbegin()->first;}
        Price_t best_ask_price() const { return asks_.empty() ? MAXIMUM_ASK : asks_.begin()->first;}
        Price_t mid_price() const { return static_cast<Price_t>((best_bid_price() + best_ask_price()) / 2);}
        Price_t spread() const { return best_ask_price() - best_bid_price(); }

        Volume_t volume_at(Side side, Price_t price) const {
            const auto& levels = side == Side::BUY ? bids_ : asks_;
            auto it = levels.find(price);
            return it == levels.end() ? 0 : it->second;
        }

        const std::map<Price_t, Volume_t>& bids() const {return bids_;}
        const std::map<Price_t, Volume_t>& asks() const {return asks_;}

    private:
        std::map<Price_t, Volume_t> bids_;
        std::map<Price_t, Volume_t> asks_;
};
