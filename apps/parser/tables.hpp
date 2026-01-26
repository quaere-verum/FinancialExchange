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

static arrow::Result<std::shared_ptr<arrow::Table>>
trade_block_to_table(const std::vector<PayloadTradeEvent>& rows)
{
    const int64_t n = static_cast<int64_t>(rows.size());
    auto* pool = arrow::default_memory_pool();

    ARROW_ASSIGN_OR_RAISE(auto seq,  make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto tid,  make_primitive_array_out<Id_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto pr,   make_primitive_array_out<Price_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto qty,  make_primitive_array_out<Volume_t>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto side, make_primitive_array_out<std::underlying_type_t<Side>>(n, pool));
    ARROW_ASSIGN_OR_RAISE(auto ts,   make_primitive_array_out<Time_t>(n, pool));

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
    }

    static const std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("trade_id",        arrow_type_for<Id_t>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("quantity",        arrow_type_for<Volume_t>(), false),
        arrow::field("taker_side",      arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    std::vector<std::shared_ptr<arrow::Array>> cols;
    cols.reserve(6);
    cols.push_back(seq.array);
    cols.push_back(tid.array);
    cols.push_back(pr.array);
    cols.push_back(qty.array);
    cols.push_back(side.array);
    cols.push_back(ts.array);

    return make_table(schema, std::move(cols));
}

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
