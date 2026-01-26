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
    if (argc < 4) {
        std::cerr << "Usage: Parser <log_dir> <run_prefix> <out_dir> [batch_rows]\n";
        return 2;
    }

    fs::path log_dir = argv[1];
    std::string run_prefix = argv[2];
    fs::path out_dir = argv[3];
    int64_t batch_rows = (argc >= 5) ? std::stoll(argv[4]) : 2'000'000; // tune as needed

    fs::path run_out = out_dir / run_prefix;

    auto in_trade  = log_dir / (run_prefix + "_trade.bin");
    auto in_plu    = log_dir / (run_prefix + "_price_level_update.bin");
    auto in_ins    = log_dir / (run_prefix + "_insert_order.bin");
    auto in_can    = log_dir / (run_prefix + "_cancel_order.bin");
    auto in_amend  = log_dir / (run_prefix + "_amend_order.bin");

    arrow::Status st;

    if (fs::exists(in_trade)) {
        st = export_one_kind<PayloadTradeEvent>(
            in_trade, run_out / "trade", batch_rows, trade_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    }

    if (fs::exists(in_plu)) {
        st = export_one_kind<PayloadPriceLevelUpdate>(
            in_plu, run_out / "price_level_update", batch_rows, plu_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
        
        st = replay_book::replay_plu_bin_to_parquet(in_plu, run_out / "order_book_stats");
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    }

    if (fs::exists(in_ins)) {
        st = export_one_kind<PayloadOrderInsertedEvent>(
            in_ins, run_out / "order_inserted", batch_rows, insert_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    }

    if (fs::exists(in_can)) {
        st = export_one_kind<PayloadOrderCancelledEvent>(
            in_can, run_out / "order_cancelled", batch_rows, cancel_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    }

    if (fs::exists(in_amend)) {
        st = export_one_kind<PayloadOrderAmendedEvent>(
            in_amend, run_out / "order_amended", batch_rows, amend_block_to_table);
        if (!st.ok()) { std::cerr << st.ToString() << "\n"; return 1; }
    }

    std::cout << "Done.\n";
    return 0;
}