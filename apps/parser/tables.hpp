#pragma once
#include <arrow/api.h>

#include <type_traits>
#include <vector>

#include "types.hpp"
#include "protocol.hpp"
#include "util.hpp"

static inline arrow::Result<std::shared_ptr<arrow::Table>> make_table(
    const std::shared_ptr<arrow::Schema>& schema,
    std::vector<std::shared_ptr<arrow::Array>>&& cols)
{
    return arrow::Table::Make(schema, std::move(cols));
}


struct TimeWeightedEwma {
    double tau_seconds = 1.0;
    double value = 0.0;
    bool initialized = false;

    underlying_or_self_t<Time_t> last_ts = {};

    void reset() { value = 0.0; initialized = false; last_ts = {}; }

    double update(double x, underlying_or_self_t<Time_t> ts, double tick_seconds) {
        if (!initialized) {
            value = x;
            initialized = true;
            last_ts = ts;
            return value;
        }
        const auto d_ticks = ts - last_ts;
        last_ts = ts;

        if (d_ticks <= 0) {
            const double alpha = 1.0;
            value = value + alpha * (x - value);
            return value;
        }

        const double dt = static_cast<double>(d_ticks) * tick_seconds;
        const double alpha = 1.0 - std::exp(-dt / tau_seconds);
        value = value + alpha * (x - value);
        return value;
    }
};

struct TradeToTableWithTimeEwmaStats {
    TimeWeightedEwma ewma_signed_vol;
    TimeWeightedEwma ewma_abs_volume;
    TimeWeightedEwma ewma_variance;

    double tick_seconds = 1e-9;

    bool price_init_ = false;
    Price_t previous_price_ = 0;

    std::shared_ptr<arrow::Schema> schema;

    TradeToTableWithTimeEwmaStats(
        double tau_signed, 
        double tau_volume,
        double tau_variance,
        double tick_seconds_
    )
        : tick_seconds(tick_seconds_)
    {
        ewma_signed_vol.tau_seconds = tau_signed;
        ewma_abs_volume.tau_seconds = tau_volume;
        ewma_variance.tau_seconds   = tau_variance;


        schema = arrow::schema({
            arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
            arrow::field("trade_id",        arrow_type_for<Id_t>(), false),
            arrow::field("price",           arrow_type_for<Price_t>(), false),
            arrow::field("quantity",        arrow_type_for<Volume_t>(), false),
            arrow::field("taker_side",      arrow_type_for<std::underlying_type_t<Side>>(), false),
            arrow::field("timestamp",       arrow_type_for<Time_t>(), false),

            arrow::field("signed_volume",   arrow::float64(), false),
            arrow::field("ewma_signed_vol", arrow::float64(), false),
            arrow::field("ewma_abs_volume", arrow::float64(), false),
            arrow::field("ewma_imbalance",  arrow::float64(), false),
            arrow::field("ewma_variance",   arrow::float64(), false),
        });
    }

    arrow::Result<std::shared_ptr<arrow::Table>>
    operator()(const std::vector<PayloadTradeEvent>& rows)
    {
        const int64_t n = static_cast<int64_t>(rows.size());
        auto* pool = arrow::default_memory_pool();

        ARROW_ASSIGN_OR_RAISE(auto seq,  make_primitive_array_out<Id_t>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto tid,  make_primitive_array_out<Id_t>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto pr,   make_primitive_array_out<Price_t>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto qty,  make_primitive_array_out<Volume_t>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto side, make_primitive_array_out<std::underlying_type_t<Side>>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto ts,   make_primitive_array_out<Time_t>(n, pool));

        ARROW_ASSIGN_OR_RAISE(auto signed_vol,  make_primitive_array_out<double>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto ewma_sv,     make_primitive_array_out<double>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto ewma_av,     make_primitive_array_out<double>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto ewma_imb,    make_primitive_array_out<double>(n, pool));
        ARROW_ASSIGN_OR_RAISE(auto ewma_var,    make_primitive_array_out<double>(n, pool));

        using UId   = underlying_or_self_t<Id_t>;
        using UPr   = underlying_or_self_t<Price_t>;
        using UVol  = underlying_or_self_t<Volume_t>;
        using UTime = underlying_or_self_t<Time_t>;
        using USide = std::underlying_type_t<Side>;

        for (int64_t i = 0; i < n; ++i) {
            const auto& r = rows[static_cast<size_t>(i)];

            seq.data[i]  = static_cast<UId>(r.sequence_number);
            tid.data[i]  = static_cast<UId>(r.trade_id);
            pr.data[i]   = static_cast<UPr>(r.price);
            qty.data[i]  = static_cast<UVol>(r.quantity);
            side.data[i] = static_cast<USide>(r.taker_side);
            ts.data[i]   = static_cast<UTime>(r.timestamp);

            const double q = static_cast<double>(static_cast<UVol>(r.quantity));
            const int sgn  = (r.taker_side == Side::BUY) ? 1 : -1;
            const double sv = static_cast<double>(sgn) * q;
            const double av = std::abs(sv);

            double r2 = 0.0;
            if (!price_init_) {
                previous_price_ = r.price;
                price_init_ = true;
            } else {
                const double p0 = static_cast<double>(previous_price_);
                const double p1 = static_cast<double>(r.price);

                const double r  = std::log(p1 / p0);
                r2 = r * r;
            }

            const UTime t = static_cast<UTime>(r.timestamp);

            const double sv_ew =  ewma_signed_vol.update(sv, t, tick_seconds);
            const double av_ew =  ewma_abs_volume.update(av, t, tick_seconds);
            const double var_ew = ewma_variance.update(r2, t, tick_seconds);

            signed_vol.data[i] = sv;
            ewma_sv.data[i]    = sv_ew;
            ewma_av.data[i]    = av_ew;
            ewma_var.data[i]   = var_ew;

            const double denom = (av_ew > 1e-12) ? av_ew : 1e-12;
            ewma_imb.data[i] = sv_ew / denom;
        }

        std::vector<std::shared_ptr<arrow::Array>> cols;
        cols.reserve(10);
        cols.push_back(seq.array);
        cols.push_back(tid.array);
        cols.push_back(pr.array);
        cols.push_back(qty.array);
        cols.push_back(side.array);
        cols.push_back(ts.array);
        cols.push_back(signed_vol.array);
        cols.push_back(ewma_sv.array);
        cols.push_back(ewma_av.array);
        cols.push_back(ewma_imb.array);
        cols.push_back(ewma_var.array);

        return arrow::Table::Make(schema, std::move(cols));
    }
};


static arrow::Result<std::shared_ptr<arrow::Table>>
plu_block_to_table(const std::vector<PayloadPriceLevelUpdate>& rows)
{
    const int64_t n = static_cast<int64_t>(rows.size());
    auto* pool = arrow::default_memory_pool();

    ARROW_ASSIGN_OR_RAISE(auto seq,  make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto side, make_primitive_array_out<std::underlying_type_t<Side>>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto pr,   make_primitive_array_out<Price_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto vol,  make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto ts,   make_primitive_array_out<Time_t>(n, pool));

    using UId   = underlying_or_self_t<Id_t>;
    using UPr   = underlying_or_self_t<Price_t>;
    using UVol  = underlying_or_self_t<Volume_t>;
    using UTime = underlying_or_self_t<Time_t>;
    using USide = std::underlying_type_t<Side>;

    for (int64_t i = 0; i < n; ++i) {
        const auto& r = rows[static_cast<size_t>(i)];
        seq.data[i]  = static_cast<UId>(r.sequence_number);
        side.data[i] = static_cast<USide>(r.side);
        pr.data[i]   = static_cast<UPr>(r.price);
        vol.data[i]  = static_cast<UVol>(r.total_volume);
        ts.data[i]   = static_cast<UTime>(r.timestamp);
    }

    static const std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("side",            arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("total_volume",    arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(5);
    cols.push_back(seq.array);
    cols.push_back(side.array);
    cols.push_back(pr.array);
    cols.push_back(vol.array);
    cols.push_back(ts.array);

    return make_table(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>>
insert_block_to_table(const std::vector<PayloadOrderInsertedEvent>& rows)
{
    const int64_t n = static_cast<int64_t>(rows.size());
    auto* pool = arrow::default_memory_pool();

    ARROW_ASSIGN_OR_RAISE(auto seq,  make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto oid,  make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto side, make_primitive_array_out<std::underlying_type_t<Side>>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto pr,   make_primitive_array_out<Price_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto qty,  make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto ts,   make_primitive_array_out<Time_t>(n, pool));

    using UId   = underlying_or_self_t<Id_t>;
    using UPr   = underlying_or_self_t<Price_t>;
    using UVol  = underlying_or_self_t<Volume_t>;
    using UTime = underlying_or_self_t<Time_t>;
    using USide = std::underlying_type_t<Side>;

    for (int64_t i = 0; i < n; ++i) {
        const auto& r = rows[static_cast<size_t>(i)];
        seq.data[i]  = static_cast<UId>(r.sequence_number);
        oid.data[i]  = static_cast<UId>(r.order_id);
        side.data[i] = static_cast<USide>(r.side);
        pr.data[i]   = static_cast<UPr>(r.price);
        qty.data[i]  = static_cast<UVol>(r.quantity);
        ts.data[i]   = static_cast<UTime>(r.timestamp);
    }

    static const std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("order_id",        arrow_type_for<Id_t>(), false),
        arrow::field("side",            arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("quantity",        arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(6);
    cols.push_back(seq.array);
    cols.push_back(oid.array);
    cols.push_back(side.array);
    cols.push_back(pr.array);
    cols.push_back(qty.array);
    cols.push_back(ts.array);

    return make_table(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>>
cancel_block_to_table(const std::vector<PayloadOrderCancelledEvent>& rows)
{
    const int64_t n = static_cast<int64_t>(rows.size());
    auto* pool = arrow::default_memory_pool();

    ARROW_ASSIGN_OR_RAISE(auto seq, make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto oid, make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto rem, make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto ts,  make_primitive_array_out<Time_t>(n, pool));

    using UId   = underlying_or_self_t<Id_t>;
    using UVol  = underlying_or_self_t<Volume_t>;
    using UTime = underlying_or_self_t<Time_t>;

    for (int64_t i = 0; i < n; ++i) {
        const auto& r = rows[static_cast<size_t>(i)];
        seq.data[i] = static_cast<UId>(r.sequence_number);
        oid.data[i] = static_cast<UId>(r.order_id);
        rem.data[i] = static_cast<UVol>(r.remaining_quantity);
        ts.data[i]  = static_cast<UTime>(r.timestamp);
    }

    static const std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("sequence_number",    arrow_type_for<Id_t>(), false),
        arrow::field("order_id",           arrow_type_for<Id_t>(), false),
        arrow::field("remaining_quantity", arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",          arrow_type_for<Time_t>(), false),
    });

    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(4);
    cols.push_back(seq.array);
    cols.push_back(oid.array);
    cols.push_back(rem.array);
    cols.push_back(ts.array);

    return make_table(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>>
amend_block_to_table(const std::vector<PayloadOrderAmendedEvent>& rows)
{
    const int64_t n = static_cast<int64_t>(rows.size());
    auto* pool = arrow::default_memory_pool();

    ARROW_ASSIGN_OR_RAISE(auto seq, make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto oid, make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto qn,  make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto qo,  make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto ts,  make_primitive_array_out<Time_t>(n, pool));

    using UId   = underlying_or_self_t<Id_t>;
    using UVol  = underlying_or_self_t<Volume_t>;
    using UTime = underlying_or_self_t<Time_t>;

    for (int64_t i = 0; i < n; ++i) {
        const auto& r = rows[static_cast<size_t>(i)];
        seq.data[i] = static_cast<UId>(r.sequence_number);
        oid.data[i] = static_cast<UId>(r.order_id);
        qn.data[i]  = static_cast<UVol>(r.quantity_new);
        qo.data[i]  = static_cast<UVol>(r.quantity_old);
        ts.data[i]  = static_cast<UTime>(r.timestamp);
    }

    static const std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("order_id",        arrow_type_for<Id_t>(), false),
        arrow::field("quantity_new",    arrow_type_for<Volume_t>(), false),
        arrow::field("quantity_old",    arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(5);
    cols.push_back(seq.array);
    cols.push_back(oid.array);
    cols.push_back(qn.array);
    cols.push_back(qo.array);
    cols.push_back(ts.array);

    return make_table(schema, std::move(cols));
}
