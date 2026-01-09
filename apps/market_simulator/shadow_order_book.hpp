#pragma once
#include <iostream>
#include "protocol.hpp"
#include "types.hpp"
#include <map>
#include <optional>

class ShadowOrderBook {
    public:
        void on_order_book_snapshot(const PayloadOrderBookSnapshot* snapshot) {
            bids_.clear();
            asks_.clear();
            for (size_t idx = 0; idx < snapshot->ask_prices.size(); ++idx) {
                Price_t ask_price = snapshot->ask_prices[idx];
                Price_t bid_price = snapshot->bid_prices[idx];
                Volume_t ask_volume = snapshot->ask_volumes[idx];
                Volume_t bid_volume = snapshot->bid_volumes[idx];
                if (ask_volume > 0) {
                    asks_[ask_price] = ask_volume;
                }
                if (bid_volume > 0) {
                    bids_[bid_price] = bid_volume;
                }
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

        inline std::optional<Price_t> best_bid_price() const {
            if (bids_.empty()) {
                return std::nullopt;
            } else {
                return bids_.rbegin()->first;
            }
        }
        inline std::optional<Price_t> best_ask_price() const {
            if (asks_.empty()) {
                return std::nullopt;
            } else {
                return asks_.begin()->first;
            }
        }
        inline std::optional<Price_t> mid_price() const { 
            const auto best_bid = best_bid_price();
            const auto best_ask = best_ask_price();
            if (!best_bid || !best_ask) {
                return std::nullopt;
            } else {
                return static_cast<Price_t>((best_bid.value() + best_ask.value()) / 2);
            }
        }
        inline std::optional<Price_t> spread() const {
            const auto best_bid = best_bid_price();
            const auto best_ask = best_ask_price();
            if (!best_bid || !best_ask) {
                return std::nullopt;
            } else {
                return static_cast<Price_t>((best_ask.value() - best_bid.value()));
            }
        }

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
