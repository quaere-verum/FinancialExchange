#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <filesystem>
#include "util.hpp"


namespace fs = std::filesystem;

struct FileChunk {
    std::vector<uint8_t> bytes;
    int64_t records = 0;
};

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

    const std::streamoff offset = static_cast<std::streamoff>(start_record * static_cast<int64_t>(sizeof(Payload)));
    ifs.seekg(offset, std::ios::beg);
    if (!ifs) {
        return arrow::Status::IOError("Failed to seek: ", path.string());
    }

    out.resize(static_cast<size_t>(n));
    ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n * sizeof(Payload)));
    if (!ifs) {
        return arrow::Status::IOError("Failed to read block: ", path.string());
    }
    return arrow::Status::OK();
}


static arrow::Status write_parquet_part(
    const std::shared_ptr<arrow::Table>& table,
    const fs::path& out_file,
    parquet::Compression::type compression = parquet::Compression::SNAPPY)
{
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    if (ec) return arrow::Status::IOError("Failed to create dirs: ", out_file.parent_path().string());

    ARROW_ASSIGN_OR_RAISE(auto outfile, arrow::io::FileOutputStream::Open(out_file.string()));

    parquet::WriterProperties::Builder props_builder;
    props_builder.compression(compression);
    auto props = props_builder.build();

    return parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, /*chunk_size=*/1024 * 1024, props);
}

template <typename Payload, typename BlockToTableFn>
static arrow::Status export_one_kind(
    const fs::path& in_file,
    const fs::path& out_dir_kind,
    int64_t batch_rows,
    BlockToTableFn&& to_table)
{
    const int64_t total = count_records_truncating<Payload>(in_file);
    if (total <= 0) {
        std::cout << "Skip (empty): " << in_file.string() << "\n";
        return arrow::Status::OK();
    }

    std::vector<Payload> block;
    int part = 0;

    for (int64_t start = 0; start < total; start += batch_rows) {
        ARROW_RETURN_NOT_OK(read_payload_block<Payload>(in_file, start, batch_rows, block));
        if (block.empty()) break;

        ARROW_ASSIGN_OR_RAISE(auto table, to_table(block));
        fs::path out_file = out_dir_kind / part_name(part++);
        ARROW_RETURN_NOT_OK(write_parquet_part(table, out_file));

        std::cout << "Wrote " << out_file.string()
                  << " rows=" << table->num_rows() << "\n";
    }

    return arrow::Status::OK();
}