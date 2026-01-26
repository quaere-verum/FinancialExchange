#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <system_error>

#include "util.hpp"

namespace fs = std::filesystem;


template <typename Payload>
static arrow::Status read_payload_block(
    std::ifstream& ifs,
    const fs::path& path_for_errors,
    int64_t start_record,
    int64_t max_records,
    int64_t total_records,
    std::vector<Payload>& out)
{
    // Fast exits
    if (start_record >= total_records) {
        out.clear();
        return arrow::Status::OK();
    }

    const int64_t n = std::min(max_records, total_records - start_record);
    if (n <= 0) {
        out.clear();
        return arrow::Status::OK();
    }

    if (static_cast<int64_t>(out.capacity()) < n) {
        out.reserve(static_cast<size_t>(n));
    }
    out.resize(static_cast<size_t>(n));

    const std::streamoff offset =
        static_cast<std::streamoff>(start_record * static_cast<int64_t>(sizeof(Payload)));

    ifs.clear(); // clear eof/fail bits before seek
    ifs.seekg(offset, std::ios::beg);
    if (!ifs) {
        return arrow::Status::IOError("Failed to seek: ", path_for_errors.string());
    }

    ifs.read(reinterpret_cast<char*>(out.data()),
             static_cast<std::streamsize>(n * static_cast<int64_t>(sizeof(Payload))));
    if (!ifs) {
        return arrow::Status::IOError("Failed to read block: ", path_for_errors.string());
    }

    return arrow::Status::OK();
}

template <typename Payload>
static arrow::Status read_payload_block(
    const fs::path& path,
    int64_t start_record,
    int64_t max_records,
    std::vector<Payload>& out)
{
    out.clear();

    const int64_t total = count_records_truncating<Payload>(path);
    if (start_record >= total) return arrow::Status::OK();

    const int64_t n = std::min(max_records, total - start_record);
    if (n <= 0) return arrow::Status::OK();

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return arrow::Status::IOError("Failed to open: ", path.string());
    }

    return read_payload_block<Payload>(ifs, path, start_record, max_records, total, out);
}

struct ParquetWriteOptions {
    parquet::Compression::type compression = parquet::Compression::SNAPPY;
    int64_t row_group_size = 1024 * 1024;
};

static arrow::Status write_parquet_part(
    const std::shared_ptr<arrow::Table>& table,
    const fs::path& out_file,
    const ParquetWriteOptions& opts = {})
{
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    if (ec) {
        return arrow::Status::IOError("Failed to create dirs: ", out_file.parent_path().string());
    }

    ARROW_ASSIGN_OR_RAISE(auto outfile, arrow::io::FileOutputStream::Open(out_file.string()));

    parquet::WriterProperties::Builder props_builder;
    props_builder.compression(opts.compression);
    auto props = props_builder.build();

    return parquet::arrow::WriteTable(
        *table,
        arrow::default_memory_pool(),
        outfile,
        /*chunk_size=*/opts.row_group_size,
        props);
}

template <typename Payload, typename BlockToTableFn>
static arrow::Status export_one_kind(
    const fs::path& in_file,
    const fs::path& out_dir_kind,
    int64_t batch_rows,
    BlockToTableFn&& to_table,
    const ParquetWriteOptions& write_opts = {})
{
    if (batch_rows <= 0) {
        return arrow::Status::Invalid("batch_rows must be > 0");
    }

    const int64_t total = count_records_truncating<Payload>(in_file);
    if (total <= 0) {
        std::cout << "Skip (empty): " << in_file.string() << "\n";
        return arrow::Status::OK();
    }

    std::ifstream ifs(in_file, std::ios::binary);
    if (!ifs) {
        return arrow::Status::IOError("Failed to open: ", in_file.string());
    }

    std::vector<Payload> block;
    block.reserve(static_cast<size_t>(std::min<int64_t>(batch_rows, total)));

    int part = 0;
    for (int64_t start = 0; start < total; start += batch_rows) {
        ARROW_RETURN_NOT_OK(read_payload_block<Payload>(
            ifs, in_file, start, batch_rows, total, block));

        if (block.empty()) break;

        ARROW_ASSIGN_OR_RAISE(auto table, to_table(block));

        fs::path out_file = out_dir_kind / part_name(part++);
        ARROW_RETURN_NOT_OK(write_parquet_part(table, out_file, write_opts));

        std::cout << "Wrote " << out_file.string()
                  << " rows=" << table->num_rows() << "\n";
    }

    return arrow::Status::OK();
}
