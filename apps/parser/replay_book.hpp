#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "protocol.hpp"
#include "types.hpp"
#include "util.hpp"

namespace replay_book {
namespace fs = std::filesystem;

class ShadowOrderBook {
public:
    explicit ShadowOrderBook(size_t /*expected_levels*/ = 100) {}

    inline void on_price_level_update(const PayloadPriceLevelUpdate* u) {
        if (!u) return;
        if (u->side == Side::BUY) {
            update_one_side(bids_, u->price, u->total_volume);
        } else {
            update_one_side(asks_, u->price, u->total_volume);
        }
    }

    inline std::optional<std::pair<Price_t, Volume_t>> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        auto it = bids_.rbegin();
        return std::make_optional(std::make_pair(it->first, it->second));
    }

    inline std::optional<std::pair<Price_t, Volume_t>> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        auto it = asks_.begin();
        return std::make_optional(std::make_pair(it->first, it->second));
    }

    inline std::optional<Price_t> best_bid_price() const {
        auto bb = best_bid();
        return bb ? std::make_optional(bb->first) : std::nullopt;
    }

    inline std::optional<Price_t> best_ask_price() const {
        auto ba = best_ask();
        return ba ? std::make_optional(ba->first) : std::nullopt;
    }

    inline Volume_t volume_at(Side side, Price_t price) const {
        const auto& m = (side == Side::BUY) ? bids_ : asks_;
        auto it = m.find(price);
        return (it == m.end()) ? static_cast<Volume_t>(0) : it->second;
    }

    inline void clear() {
        bids_.clear();
        asks_.clear();
    }

private:
    using Map = std::map<Price_t, Volume_t>;

    static inline void update_one_side(Map& m, Price_t price, Volume_t total_volume) {
        if (total_volume == 0) {
            m.erase(price);
        } else {
            m.insert_or_assign(price, total_volume);
        }
    }

    Map bids_;
    Map asks_;
};

class BookStateWriter {
public:
    struct Config {
        int64_t rows_per_part = 2'000'000;
        parquet::Compression::type compression = parquet::Compression::SNAPPY;
        int64_t row_group_size = 1'000'000;
    };

    BookStateWriter(fs::path out_dir, Config cfg)
        : out_dir_(std::move(out_dir)),
          cfg_(cfg),
          pool_(arrow::default_memory_pool())
    {
        if (cfg_.rows_per_part <= 0) cfg_.rows_per_part = 1;
        if (cfg_.row_group_size <= 0) cfg_.row_group_size = 1;

        init_schema();
        reset_part_storage();
    }

    arrow::Status append(
        Time_t ts,
        Id_t sequence_number,
        const std::optional<Price_t>& best_bid,
        const std::optional<Price_t>& best_ask,
        Volume_t best_bid_vol,
        Volume_t best_ask_vol)
    {
        const int64_t i = rows_in_part_;
        if (i >= cfg_.rows_per_part) {
            ARROW_RETURN_NOT_OK(flush_part());
        }

        const int64_t idx = rows_in_part_;

        ts_.data[idx]  = static_cast<underlying_or_self_t<Time_t>>(ts);
        seq_.data[idx] = static_cast<underlying_or_self_t<Id_t>>(sequence_number);

        bb_vol_.data[idx] = static_cast<underlying_or_self_t<Volume_t>>(best_bid_vol);
        ba_vol_.data[idx] = static_cast<underlying_or_self_t<Volume_t>>(best_ask_vol);

        // Nullable best bid/ask prices
        if (best_bid) {
            bb_.data[idx] = static_cast<underlying_or_self_t<Price_t>>(*best_bid);
            set_valid(bb_.valid_data, idx);
        } else {
            clear_valid(bb_.valid_data, idx);
        }

        if (best_ask) {
            ba_.data[idx] = static_cast<underlying_or_self_t<Price_t>>(*best_ask);
            set_valid(ba_.valid_data, idx);
        } else {
            clear_valid(ba_.valid_data, idx);
        }

        // Booleans (non-nullable)
        const bool valid_book = best_bid.has_value() && best_ask.has_value() && (*best_ask >= *best_bid);
        set_bool(valid_.bits, idx, valid_book);

        bool crossed = false;
        if (best_bid && best_ask) {
            const auto bb = static_cast<underlying_or_self_t<Price_t>>(*best_bid);
            const auto ba = static_cast<underlying_or_self_t<Price_t>>(*best_ask);
            crossed = (ba < bb);

            if (!crossed) {
                // spread_ticks nullable, but becomes valid here
                const auto spread_u = ba - bb;
                spread_.data[idx] = static_cast<int32_t>(spread_u);
                set_valid(spread_.valid_data, idx);

                // mid_px2_ticks nullable, valid here
                mid_px2_.data[idx] = static_cast<underlying_or_self_t<MidPx2_t>>(static_cast<MidPx2_t>(bb + ba));
                set_valid(mid_px2_.valid_data, idx);
            } else {
                clear_valid(spread_.valid_data, idx);
                clear_valid(mid_px2_.valid_data, idx);
            }
        } else {
            clear_valid(spread_.valid_data, idx);
            clear_valid(mid_px2_.valid_data, idx);
        }

        set_bool(crossed_.bits, idx, crossed);

        ++rows_in_part_;
        if (rows_in_part_ >= cfg_.rows_per_part) {
            return flush_part();
        }

        return arrow::Status::OK();
    }

    arrow::Status finish() {
        if (rows_in_part_ > 0) return flush_part();
        return arrow::Status::OK();
    }

private:
    using MidPx2_t = Price_t;

    static inline void set_valid(uint8_t* bitmap, int64_t i) {
        bitmap[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }
    static inline void clear_valid(uint8_t* bitmap, int64_t i) {
        bitmap[i >> 3] &= static_cast<uint8_t>(~(1u << (i & 7)));
    }

    static inline int64_t bitmap_nbytes(int64_t nbits) {
        return (nbits + 7) / 8;
    }

    template <typename T>
    struct Col {
        using U = underlying_or_self_t<T>;
        std::shared_ptr<arrow::Buffer> values;
        U* data = nullptr;
        std::shared_ptr<arrow::Buffer> valid;
        uint8_t* valid_data = nullptr;
    };

    template <typename T>
    arrow::Result<Col<T>> alloc_col(int64_t cap_rows, bool nullable) {
        using U = underlying_or_self_t<T>;
        static_assert(std::is_integral_v<U>, "BookStateWriter requires integral/enum types");

        Col<T> c;

        ARROW_ASSIGN_OR_RAISE(c.values,
                              arrow::AllocateBuffer(cap_rows * static_cast<int64_t>(sizeof(U)), pool_));
        c.data = reinterpret_cast<U*>(c.values->mutable_data());

        if (nullable) {
            ARROW_ASSIGN_OR_RAISE(c.valid, arrow::AllocateBuffer(bitmap_nbytes(cap_rows), pool_));
            c.valid_data = c.valid->mutable_data();
            std::memset(c.valid_data, 0, static_cast<size_t>(c.valid->size()));
        }

        return c;
    }

    struct BoolCol {
        std::shared_ptr<arrow::Buffer> values; // bitmap
        uint8_t* bits = nullptr;
    };

    arrow::Result<BoolCol> alloc_bool_col(int64_t cap_rows) {
        BoolCol c;
        ARROW_ASSIGN_OR_RAISE(c.values, arrow::AllocateBuffer(bitmap_nbytes(cap_rows), pool_));
        c.bits = c.values->mutable_data();
        std::memset(c.bits, 0, static_cast<size_t>(c.values->size()));
        return c;
    }

    static inline void set_bool(uint8_t* bits, int64_t i, bool v) {
        const uint8_t mask = static_cast<uint8_t>(1u << (i & 7));
        uint8_t& byte = bits[i >> 3];
        if (v) byte |= mask;
        else   byte &= static_cast<uint8_t>(~mask);
    }

    void init_schema() {
        schema_ = arrow::schema({
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
    }

    void reset_part_storage() {
        const int64_t cap = cfg_.rows_per_part;

        ts_     = alloc_col<Time_t>(cap, /*nullable=*/false).ValueOrDie();
        seq_    = alloc_col<Id_t>(cap,   /*nullable=*/false).ValueOrDie();
        bb_     = alloc_col<Price_t>(cap,/*nullable=*/true).ValueOrDie();
        ba_     = alloc_col<Price_t>(cap,/*nullable=*/true).ValueOrDie();
        bb_vol_ = alloc_col<Volume_t>(cap,/*nullable=*/false).ValueOrDie();
        ba_vol_ = alloc_col<Volume_t>(cap,/*nullable=*/false).ValueOrDie();
        spread_ = alloc_col<int32_t>(cap, /*nullable=*/true).ValueOrDie();
        mid_px2_= alloc_col<MidPx2_t>(cap,/*nullable=*/true).ValueOrDie();

        valid_   = alloc_bool_col(cap).ValueOrDie();
        crossed_ = alloc_bool_col(cap).ValueOrDie();

        rows_in_part_ = 0;
    }

    arrow::Result<std::shared_ptr<arrow::Table>> finish_table() {
        const int64_t n = rows_in_part_;

        auto make_prim = [&](auto& col, const std::shared_ptr<arrow::DataType>& dt, bool nullable) {
            std::vector<std::shared_ptr<arrow::Buffer>> bufs;
            bufs.reserve(2);
            if (nullable) {
                bufs.push_back(col.valid);
            } else {
                bufs.push_back(nullptr);
            }
            bufs.push_back(col.values);

            auto ad = arrow::ArrayData::Make(dt, n, std::move(bufs), /*null_count=*/-1);
            return arrow::MakeArray(ad);
        };

        auto make_bool = [&](const BoolCol& b) {
            auto ad = arrow::ArrayData::Make(arrow::boolean(), n, {nullptr, b.values}, /*null_count=*/0);
            return arrow::MakeArray(ad);
        };

        std::vector<std::shared_ptr<arrow::Array>> cols;
        cols.reserve(10);

        cols.push_back(make_prim(ts_,      arrow_type_for<Time_t>(),   false));
        cols.push_back(make_prim(seq_,     arrow_type_for<Id_t>(),     false));
        cols.push_back(make_prim(bb_,      arrow_type_for<Price_t>(),  true));
        cols.push_back(make_prim(ba_,      arrow_type_for<Price_t>(),  true));
        cols.push_back(make_prim(bb_vol_,  arrow_type_for<Volume_t>(), false));
        cols.push_back(make_prim(ba_vol_,  arrow_type_for<Volume_t>(), false));
        cols.push_back(make_bool(valid_));
        cols.push_back(make_bool(crossed_));
        cols.push_back(make_prim(spread_,  arrow::int32(),             true));
        cols.push_back(make_prim(mid_px2_, arrow_type_for<MidPx2_t>(), true));

        return arrow::Table::Make(schema_, std::move(cols));
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

        ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(*table, pool_, sink, cfg_.row_group_size, props));

        std::cout << "Wrote " << out_file.string()
                  << " rows=" << rows_in_part_ << "\n";

        reset_part_storage();
        return arrow::Status::OK();
    }

private:
    fs::path out_dir_;
    Config cfg_;
    arrow::MemoryPool* pool_{nullptr};

    std::shared_ptr<arrow::Schema> schema_;
    int part_idx_ = 0;
    int64_t rows_in_part_ = 0;

    Col<Time_t>   ts_;
    Col<Id_t>     seq_;
    Col<Price_t>  bb_;
    Col<Price_t>  ba_;
    Col<Volume_t> bb_vol_;
    Col<Volume_t> ba_vol_;
    Col<int32_t>  spread_;
    Col<MidPx2_t> mid_px2_;

    BoolCol valid_;
    BoolCol crossed_;
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

    BookStateWriter writer(out_run_dir, cfg);
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

        for (int64_t i = 0; i < to_read; ++i) {
            const auto& u = buf[static_cast<size_t>(i)];
            book.on_price_level_update(&u);

            const auto bb = book.best_bid(); // {price, vol}
            const auto ba = book.best_ask(); // {price, vol}

            std::optional<Price_t> bbp;
            std::optional<Price_t> bap;
            Volume_t bbv = 0;
            Volume_t bav = 0;

            if (bb) { bbp = bb->first; bbv = bb->second; }
            if (ba) { bap = ba->first; bav = ba->second; }

            ARROW_RETURN_NOT_OK(writer.append(
                u.timestamp,
                u.sequence_number,
                bbp,
                bap,
                bbv,
                bav));
        }

        processed += to_read;
    }

    return writer.finish();
}

} // namespace replay_book
