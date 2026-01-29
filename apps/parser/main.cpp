#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <arrow/api.h>
#include "protocol.hpp"
#include "write.hpp"
#include "tables.hpp"
#include "replay_book.hpp"


namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: Parser <log_dir> <out_dir> [batch_rows]\n";
        return 2;
    }

    fs::path log_dir = argv[1];
    fs::path out_dir = argv[2];
    int64_t batch_rows = (argc >= 4) ? std::stoll(argv[3]) : 2'000'000; // tune as needed

    fs::path run_out = out_dir;

    std::error_code ec;
    fs::create_directories(run_out, ec);
    if (ec) {
        std::cerr << "Failed to create output directory: "
                << run_out << " (" << ec.message() << ")\n";
        return 1;
    }

    auto in_trade  = log_dir / "trade.bin";
    auto in_plu    = log_dir / "price_level_update.bin";
    auto in_ins    = log_dir / "insert_order.bin";
    auto in_can    = log_dir / "cancel_order.bin";
    auto in_amend  = log_dir / "amend_order.bin";

    arrow::Status st;

    if (fs::exists(in_trade)) {
        TradeToTableWithTimeEwmaStats to_table(/*tau_sign=*/3.0, /*tau_volume=*/2.0, /*tau_variance=*/1.0, /*tick_seconds=*/1e-9);
        st = export_one_kind<PayloadTradeEvent>(
            in_trade, run_out / "trade", batch_rows, to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    } else {
        std::cout << "Could not find " << in_trade.string() << ", skipping trades.\n";
    }

    if (fs::exists(in_plu)) {
        st = export_one_kind<PayloadPriceLevelUpdate>(
            in_plu, run_out / "price_level_update", batch_rows, plu_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
        
        st = replay_book::replay_plu_bin_to_parquet(
            in_plu, run_out / "order_book_stats", 
            batch_rows, 
            {
                batch_rows,
                parquet::Compression::SNAPPY,
                batch_rows / 2
            }
        );
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    } else {
        std::cout << "Could not find " << in_plu.string() << ", skipping price level updates.\n";
    }

    if (fs::exists(in_ins)) {
        st = export_one_kind<PayloadOrderInsertedEvent>(
            in_ins, run_out / "order_inserted", batch_rows, insert_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    } else {
        std::cout << "Could not find " << in_ins.string() << ", skipping inserts.\n";
    }

    if (fs::exists(in_can)) {
        st = export_one_kind<PayloadOrderCancelledEvent>(
            in_can, run_out / "order_cancelled", batch_rows, cancel_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    } else {
        std::cout << "Could not find " << in_can.string() << ", skipping cancels.\n";
    }

    if (fs::exists(in_amend)) {
        st = export_one_kind<PayloadOrderAmendedEvent>(
            in_amend, run_out / "order_amended", batch_rows, amend_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    } else {
        std::cout << "Could not find " << in_amend.string() << ", skipping amends.\n";
    }

    std::cout << "Done.\n";
    return 0;
}