#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/builder.h>
#include "types.hpp"
#include "protocol.hpp"
#include "endian.hpp"
#include "util.hpp"


static arrow::Result<std::shared_ptr<arrow::Table>> trade_block_to_table(const std::vector<PayloadTradeEvent>& rows) {
    std::vector<Id_t> seq(rows.size());
    std::vector<Id_t> trade_id(rows.size());
    std::vector<Price_t> price(rows.size());
    std::vector<Volume_t> qty(rows.size());
    std::vector<std::underlying_type_t<Side>> taker_side(rows.size());
    std::vector<Time_t> ts(rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        seq[i]       = r.sequence_number;
        trade_id[i]  = r.trade_id;
        price[i]     = r.price;
        qty[i]       = r.quantity;
        taker_side[i]= static_cast<std::underlying_type_t<Side>>(r.taker_side);
        ts[i]        = r.timestamp;
    }

    auto schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("trade_id",        arrow_type_for<Id_t>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("quantity",        arrow_type_for<Volume_t>(), false),
        arrow::field("taker_side",      arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    ARROW_ASSIGN_OR_RAISE(auto a_seq,   make_primitive_array(seq));
    ARROW_ASSIGN_OR_RAISE(auto a_tid,   make_primitive_array(trade_id));
    ARROW_ASSIGN_OR_RAISE(auto a_price, make_primitive_array(price));
    ARROW_ASSIGN_OR_RAISE(auto a_qty,   make_primitive_array(qty));
    ARROW_ASSIGN_OR_RAISE(auto a_side,  make_primitive_array(taker_side));
    ARROW_ASSIGN_OR_RAISE(auto a_ts,    make_primitive_array(ts));
    

    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    cols.reserve(6);
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_tid));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_price));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_qty));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_side));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));

    return arrow::Table::Make(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>> plu_block_to_table(const std::vector<PayloadPriceLevelUpdate>& rows) {
    std::vector<Id_t> seq(rows.size());
    std::vector<std::underlying_type_t<Side>> side(rows.size());
    std::vector<Price_t> price(rows.size());
    std::vector<Volume_t> total_vol(rows.size());
    std::vector<Time_t> ts(rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        seq[i]       = r.sequence_number;
        side[i]      = static_cast<std::underlying_type_t<Side>>(r.side);
        price[i]     = r.price;
        total_vol[i] = r.total_volume;
        ts[i]        = r.timestamp;
    }

    auto schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("side",            arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("total_volume",    arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    ARROW_ASSIGN_OR_RAISE(auto a_seq,   make_primitive_array(seq));
    ARROW_ASSIGN_OR_RAISE(auto a_side,  make_primitive_array(side));
    ARROW_ASSIGN_OR_RAISE(auto a_price, make_primitive_array(price));
    ARROW_ASSIGN_OR_RAISE(auto a_vol,   make_primitive_array(total_vol));
    ARROW_ASSIGN_OR_RAISE(auto a_ts,    make_primitive_array(ts));

    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    cols.reserve(5);
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_side));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_price));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_vol));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));

    return arrow::Table::Make(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>> insert_block_to_table(const std::vector<PayloadOrderInsertedEvent>& rows) {
    std::vector<Id_t> seq(rows.size());
    std::vector<Id_t> order_id(rows.size());
    std::vector<std::underlying_type_t<Side>> side(rows.size());
    std::vector<Price_t> price(rows.size());
    std::vector<Volume_t> qty(rows.size());
    std::vector<Time_t> ts(rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        seq[i]     = r.sequence_number;
        order_id[i]= r.order_id;
        side[i]    = static_cast<std::underlying_type_t<Side>>(r.side);
        price[i]   = r.price;
        qty[i]     = r.quantity;
        ts[i]      = r.timestamp;
    }

    auto schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("order_id",        arrow_type_for<Id_t>(), false),
        arrow::field("side",            arrow_type_for<std::underlying_type_t<Side>>(), false),
        arrow::field("price",           arrow_type_for<Price_t>(), false),
        arrow::field("quantity",        arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    ARROW_ASSIGN_OR_RAISE(auto a_seq,   make_primitive_array(seq));
    ARROW_ASSIGN_OR_RAISE(auto a_oid,   make_primitive_array(order_id));
    ARROW_ASSIGN_OR_RAISE(auto a_side,  make_primitive_array(side));
    ARROW_ASSIGN_OR_RAISE(auto a_price, make_primitive_array(price));
    ARROW_ASSIGN_OR_RAISE(auto a_qty,   make_primitive_array(qty));
    ARROW_ASSIGN_OR_RAISE(auto a_ts,    make_primitive_array(ts));

    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    cols.reserve(6);
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_oid));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_side));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_price));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_qty));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));

    return arrow::Table::Make(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>> cancel_block_to_table(const std::vector<PayloadOrderCancelledEvent>& rows) {
    std::vector<Id_t> seq(rows.size());
    std::vector<Id_t> order_id(rows.size());
    std::vector<Volume_t> remaining(rows.size());
    std::vector<Time_t> ts(rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        seq[i]       = r.sequence_number;
        order_id[i]  = r.order_id;
        remaining[i] = r.remaining_quantity;
        ts[i]        = r.timestamp;
    }

    auto schema = arrow::schema({
        arrow::field("sequence_number",    arrow_type_for<Id_t>(), false),
        arrow::field("order_id",           arrow_type_for<Id_t>(), false),
        arrow::field("remaining_quantity", arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",          arrow_type_for<Time_t>(), false),
    });

    ARROW_ASSIGN_OR_RAISE(auto a_seq,  make_primitive_array(seq));
    ARROW_ASSIGN_OR_RAISE(auto a_oid,  make_primitive_array(order_id));
    ARROW_ASSIGN_OR_RAISE(auto a_rem,  make_primitive_array(remaining));
    ARROW_ASSIGN_OR_RAISE(auto a_ts,   make_primitive_array(ts));

    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    cols.reserve(4);
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_oid));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_rem));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));

    return arrow::Table::Make(schema, std::move(cols));
}

static arrow::Result<std::shared_ptr<arrow::Table>> amend_block_to_table(const std::vector<PayloadOrderAmendedEvent>& rows) {
    std::vector<Id_t> seq(rows.size());
    std::vector<Id_t> order_id(rows.size());
    std::vector<Volume_t> q_new(rows.size());
    std::vector<Volume_t> q_old(rows.size());
    std::vector<Time_t> ts(rows.size());

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        seq[i]      = r.sequence_number;
        order_id[i] = r.order_id;
        q_new[i]    = r.quantity_new;
        q_old[i]    = r.quantity_old;
        ts[i]       = r.timestamp;
    }

    auto schema = arrow::schema({
        arrow::field("sequence_number", arrow_type_for<Id_t>(), false),
        arrow::field("order_id",        arrow_type_for<Id_t>(), false),
        arrow::field("quantity_new",    arrow_type_for<Volume_t>(), false),
        arrow::field("quantity_old",    arrow_type_for<Volume_t>(), false),
        arrow::field("timestamp",       arrow_type_for<Time_t>(), false),
    });

    ARROW_ASSIGN_OR_RAISE(auto a_seq,  make_primitive_array(seq));
    ARROW_ASSIGN_OR_RAISE(auto a_oid,  make_primitive_array(order_id));
    ARROW_ASSIGN_OR_RAISE(auto a_new,  make_primitive_array(q_new));
    ARROW_ASSIGN_OR_RAISE(auto a_old,  make_primitive_array(q_old));
    ARROW_ASSIGN_OR_RAISE(auto a_ts,   make_primitive_array(ts));

    std::vector<std::shared_ptr<arrow::ChunkedArray>> cols;
    cols.reserve(5);
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_seq));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_oid));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_new));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_old));
    cols.push_back(std::make_shared<arrow::ChunkedArray>(a_ts));

    return arrow::Table::Make(schema, std::move(cols));

}