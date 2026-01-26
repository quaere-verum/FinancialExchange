#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "protocol.hpp"
#include "types.hpp"
#include "util.hpp"

namespace replay_book {
namespace fs = std::filesystem;

class ShadowOrderBook {
public:
    explicit ShadowOrderBook(size_t expected_levels = 100) {
        bid_vol_.reserve(expected_levels);
        ask_vol_.reserve(expected_levels);
    }

    inline void on_price_level_update(const PayloadPriceLevelUpdate* u) {
        if (!u) return;
        if (u->side == Side::BUY) {
            update_one_side(bid_vol_, bid_heap_, u->price, u->total_volume);
        } else {
            update_one_side(ask_vol_, ask_heap_, u->price, u->total_volume);
        }
    }

    inline std::optional<Price_t> best_bid_price() { return best_price(bid_vol_, bid_heap_); }
    inline std::optional<Price_t> best_ask_price() { return best_price(ask_vol_, ask_heap_); }

    inline Volume_t volume_at(Side side, Price_t price) const {
        const auto& m = (side == Side::BUY) ? bid_vol_ : ask_vol_;
        auto it = m.find(price);
        return (it == m.end()) ? static_cast<Volume_t>(0) : it->second;
    }

    inline void clear() {
        bid_vol_.clear();
        ask_vol_.clear();
        bid_heap_ = BidHeap{};
        ask_heap_ = AskHeap{};
    }

private:
    using Map = std::unordered_map<Price_t, Volume_t>;
    using BidHeap = std::priority_queue<Price_t>;
    using AskHeap = std::priority_queue<Price_t, std::vector<Price_t>, std::greater<Price_t>>;

    template <typename Heap>
    static inline void update_one_side(Map& m, Heap& heap, Price_t price, Volume_t total_volume) {
        if (total_volume == 0) {
            m.erase(price);
        } else {
            m[price] = total_volume;
            heap.push(price);
        }
    }

    template <typename Heap>
    static inline std::optional<Price_t> best_price(Map& m, Heap& heap) {
        while (!heap.empty()) {
            const Price_t p = heap.top();
            auto it = m.find(p);
            if (it != m.end() && it->second > 0) return p;
            heap.pop();
        }
        return std::nullopt;
    }

    Map bid_vol_;
    Map ask_vol_;
    BidHeap bid_heap_;
    AskHeap ask_heap_;
};


class BookStateWriter {
public:
    struct Config {
        int64_t rows_per_part = 2'000'000;
        parquet::Compression::type compression = parquet::Compression::SNAPPY;
        int64_t row_group_size = 1'000'000;
    };

    BookStateWriter(fs::path out_dir, Config cfg)
        : out_dir_(std::move(out_dir)), cfg_(cfg), pool_(arrow::default_memory_pool()) {
        reset_builders();
    }

    arrow::Status append(
        Time_t ts,
        Id_t sequence_number,
        const std::optional<Price_t>& best_bid,
        const std::optional<Price_t>& best_ask,
        Volume_t best_bid_vol,
        Volume_t best_ask_vol)
    {
        ARROW_RETURN_NOT_OK(append_numeric<Time_t>(b_ts_.get(), ts));
        ARROW_RETURN_NOT_OK(append_numeric<Id_t>(b_seq_.get(), sequence_number));

        if (best_bid) ARROW_RETURN_NOT_OK(append_numeric<Price_t>(b_bb_.get(), *best_bid));
        else          ARROW_RETURN_NOT_OK(b_bb_->AppendNull());

        if (best_ask) ARROW_RETURN_NOT_OK(append_numeric<Price_t>(b_ba_.get(), *best_ask));
        else          ARROW_RETURN_NOT_OK(b_ba_->AppendNull());

        ARROW_RETURN_NOT_OK(append_numeric<Volume_t>(b_bb_vol_.get(), best_bid_vol));
        ARROW_RETURN_NOT_OK(append_numeric<Volume_t>(b_ba_vol_.get(), best_ask_vol));

        const bool valid = best_bid.has_value() && best_ask.has_value() && (*best_ask >= *best_bid);
        ARROW_RETURN_NOT_OK(b_valid_->Append(valid));

        if (best_bid && best_ask) {
            const auto bb = static_cast<underlying_or_self_t<Price_t>>(*best_bid);
            const auto ba = static_cast<underlying_or_self_t<Price_t>>(*best_ask);
            const bool crossed = (ba < bb);
            ARROW_RETURN_NOT_OK(b_crossed_->Append(crossed));

            if (!crossed) {
                const auto spread_u = ba - bb;
                ARROW_RETURN_NOT_OK(b_spread_->Append(static_cast<int32_t>(spread_u)));
                ARROW_RETURN_NOT_OK(append_numeric<MidPx2_t>(b_mid_px2_.get(), static_cast<MidPx2_t>(bb + ba)));
            } else {
                ARROW_RETURN_NOT_OK(b_spread_->AppendNull());
                ARROW_RETURN_NOT_OK(b_mid_px2_->AppendNull());
            }
        } else {
            ARROW_RETURN_NOT_OK(b_crossed_->Append(false));
            ARROW_RETURN_NOT_OK(b_spread_->AppendNull());
            ARROW_RETURN_NOT_OK(b_mid_px2_->AppendNull());
        }

        ++rows_in_part_;
        if (rows_in_part_ >= cfg_.rows_per_part) return flush_part();
        return arrow::Status::OK();
    }

    arrow::Status finish() {
        if (rows_in_part_ > 0) return flush_part();
        return arrow::Status::OK();
    }

private:
    using MidPx2_t = uint64_t;

    void reset_builders() {
        b_ts_     = make_numeric_builder<Time_t>(pool_);
        b_seq_    = make_numeric_builder<Id_t>(pool_);
        b_bb_     = make_numeric_builder<Price_t>(pool_);
        b_ba_     = make_numeric_builder<Price_t>(pool_);
        b_bb_vol_ = make_numeric_builder<Volume_t>(pool_);
        b_ba_vol_ = make_numeric_builder<Volume_t>(pool_);

        b_valid_   = std::make_unique<arrow::BooleanBuilder>(pool_);
        b_crossed_ = std::make_unique<arrow::BooleanBuilder>(pool_);
        b_spread_  = std::make_unique<arrow::Int32Builder>(pool_);
        b_mid_px2_ = make_numeric_builder<MidPx2_t>(pool_);

        const int64_t r = cfg_.rows_per_part;
        (void)b_ts_->Reserve(r);
        (void)b_seq_->Reserve(r);
        (void)b_bb_->Reserve(r);
        (void)b_ba_->Reserve(r);
        (void)b_bb_vol_->Reserve(r);
        (void)b_ba_vol_->Reserve(r);
        (void)b_valid_->Reserve(r);
        (void)b_crossed_->Reserve(r);
        (void)b_spread_->Reserve(r);
        (void)b_mid_px2_->Reserve(r);

        rows_in_part_ = 0;
    }

    arrow::Result<std::shared_ptr<arrow::Table>> finish_table() {
        std::shared_ptr<arrow::Array> a_ts, a_seq, a_bb, a_ba, a_bb_vol, a_ba_vol;
        std::shared_ptr<arrow::Array> a_valid, a_crossed, a_spread, a_mid_px2;

        ARROW_RETURN_NOT_OK(b_ts_->Finish(&a_ts));
        ARROW_RETURN_NOT_OK(b_seq_->Finish(&a_seq));
        ARROW_RETURN_NOT_OK(b_bb_->Finish(&a_bb));
        ARROW_RETURN_NOT_OK(b_ba_->Finish(&a_ba));
        ARROW_RETURN_NOT_OK(b_bb_vol_->Finish(&a_bb_vol));
        ARROW_RETURN_NOT_OK(b_ba_vol_->Finish(&a_ba_vol));
        ARROW_RETURN_NOT_OK(b_valid_->Finish(&a_valid));
        ARROW_RETURN_NOT_OK(b_crossed_->Finish(&a_crossed));
        ARROW_RETURN_NOT_OK(b_spread_->Finish(&a_spread));
        ARROW_RETURN_NOT_OK(b_mid_px2_->Finish(&a_mid_px2));

        auto schema = arrow::schema({
            arrow::field("timestamp",        arrow_type_for<Time_t>(),  false),
            arrow::field("sequence_number",  arrow_type_for<Id_t>(),    false),

            arrow::field("best_bid_price",   arrow_type_for<Price_t>(), true),
            arrow::field("best_ask_price",   arrow_type_for<Price_t>(), true),

            arrow::field("best_bid_volume",  arrow_type_for<Volume_t>(), false),
            arrow::field("best_ask_volume",  arrow_type_for<Volume_t>(), false),

            arrow::field("book_valid",       arrow::boolean(), false),
            arrow::field("is_crossed",       arrow::boolean(), false),

            arrow::field("spread_ticks",     arrow::int32(),   true),
            arrow::field("mid_px2_ticks",    arrow_type_for<MidPx2_t>(), true),
        });

        std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
        cols.reserve(10);
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_bb));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ba));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_bb_vol));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ba_vol));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_valid));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_crossed));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_spread));
        cols.push_back(std::make_shared<arrow::ChunkedArray>(a_mid_px2));

        return arrow::Table::Make(schema, std::move(cols));
    }

    arrow::Status flush_part() {
        ARROW_ASSIGN_OR_RAISE(auto table, finish_table());

        std::error_code ec;
        fs::create_directories(out_dir_, ec);
        if (ec) return arrow::Status::IOError("Failed to create output directory: ", out_dir_.string());

        const fs::path out_file = out_dir_ / part_name(part_idx_++);
        ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::FileOutputStream::Open(out_file.string()));

        parquet::WriterProperties::Builder props_builder;
        props_builder.compression(cfg_.compression);
        auto props = props_builder.build();

        const int64_t rg = std::max<int64_t>(1, cfg_.row_group_size);
        ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(*table, pool_, sink, rg, props));

        reset_builders();
        return arrow::Status::OK();
    }

private:
    fs::path out_dir_;
    Config cfg_;
    arrow::MemoryPool* pool_{nullptr};

    int part_idx_ = 0;
    int64_t rows_in_part_ = 0;

    std::unique_ptr<arrow::ArrayBuilder> b_ts_;
    std::unique_ptr<arrow::ArrayBuilder> b_seq_;
    std::unique_ptr<arrow::ArrayBuilder> b_bb_;
    std::unique_ptr<arrow::ArrayBuilder> b_ba_;
    std::unique_ptr<arrow::ArrayBuilder> b_bb_vol_;
    std::unique_ptr<arrow::ArrayBuilder> b_ba_vol_;

    std::unique_ptr<arrow::BooleanBuilder> b_valid_;
    std::unique_ptr<arrow::BooleanBuilder> b_crossed_;
    std::unique_ptr<arrow::Int32Builder>   b_spread_;
    std::unique_ptr<arrow::ArrayBuilder>   b_mid_px2_;
};

inline arrow::Status replay_plu_bin_to_parquet(
    const fs::path& plu_bin_path,
    const fs::path& out_run_dir,
    const BookStateWriter::Config& cfg = {})
{
    if (!fs::exists(plu_bin_path)) {
        return arrow::Status::IOError("PLU file not found: ", plu_bin_path.string());
    }

    const int64_t total = count_records_truncating<PayloadPriceLevelUpdate>(plu_bin_path);
    if (total <= 0) return arrow::Status::OK();

    const fs::path out_dir = out_run_dir / "book_state";
    BookStateWriter writer(out_dir, cfg);
    ShadowOrderBook book(100);

    std::ifstream ifs(plu_bin_path, std::ios::binary);
    if (!ifs) return arrow::Status::IOError("Failed to open PLU file: ", plu_bin_path.string());

    constexpr int64_t kBlockRecs = 1'000'000;
    const int64_t buf_cap = std::min<int64_t>(kBlockRecs, total);

    std::vector<PayloadPriceLevelUpdate> buf;
    buf.resize(static_cast<size_t>(buf_cap));

    int64_t processed = 0;
    while (processed < total) {
        const int64_t remaining = total - processed;
        const int64_t to_read = std::min<int64_t>(buf_cap, remaining);

        ifs.read(reinterpret_cast<char*>(buf.data()),
                 static_cast<std::streamsize>(to_read * sizeof(PayloadPriceLevelUpdate)));
        if (!ifs) return arrow::Status::IOError("Read error while streaming PLU file");

        // IMPORTANT: iterate only over records actually read
        for (int64_t i = 0; i < to_read; ++i) {
            auto& u = buf[static_cast<size_t>(i)];

            book.on_price_level_update(&u);

            const auto bb = book.best_bid_price();
            const auto ba = book.best_ask_price();

            Volume_t bbv = 0;
            Volume_t bav = 0;
            if (bb) bbv = book.volume_at(Side::BUY, *bb);
            if (ba) bav = book.volume_at(Side::SELL, *ba);

            ARROW_RETURN_NOT_OK(writer.append(
                u.timestamp,
                u.sequence_number,
                bb,
                ba,
                bbv,
                bav));
        }

        processed += to_read;
    }

    return writer.finish();
}

} // namespace replay_book
