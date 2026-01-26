#pragma once
#include <type_traits>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <filesystem>
#include <arrow/api.h>

namespace fs = std::filesystem;

template <typename T, bool IsEnum = std::is_enum_v<T>>
struct underlying_or_self { using type = T; };

template <typename T>
struct underlying_or_self<T, true> { using type = std::underlying_type_t<T>; };

template <typename T>
using underlying_or_self_t = typename underlying_or_self<T>::type;

template <typename T>
std::shared_ptr<arrow::DataType> arrow_type_for() {
    using U = underlying_or_self_t<T>;
    static_assert(std::is_integral_v<U>, "Only integral/enum types supported");

    constexpr bool signed_ = std::is_signed_v<U>;
    constexpr size_t sz = sizeof(U);

    if constexpr (sz == 1) return signed_ ? arrow::int8()  : arrow::uint8();
    if constexpr (sz == 2) return signed_ ? arrow::int16() : arrow::uint16();
    if constexpr (sz == 4) return signed_ ? arrow::int32() : arrow::uint32();
    if constexpr (sz == 8) return signed_ ? arrow::int64() : arrow::uint64();

    return arrow::binary();
}

template <typename T>
struct PrimitiveArrayOut {
    using U = underlying_or_self_t<T>;
    std::shared_ptr<arrow::Array> array;
    U* data = nullptr;
};

template <typename T>
static arrow::Result<PrimitiveArrayOut<T>> make_primitive_array_out(
    int64_t length,
    arrow::MemoryPool* pool = arrow::default_memory_pool())
{
    using U = underlying_or_self_t<T>;
    static_assert(std::is_integral_v<U>, "integral/enum only");
    static_assert(std::is_unsigned_v<U>, "make_primitive_array_out assumes unsigned values");

    if (length < 0) return arrow::Status::Invalid("Negative length");

    ARROW_ASSIGN_OR_RAISE(auto values, arrow::AllocateBuffer(length * static_cast<int64_t>(sizeof(U)), pool));

    auto data = arrow::ArrayData::Make(arrow_type_for<U>(), length, {nullptr, values});

    PrimitiveArrayOut<T> out;
    out.array = arrow::MakeArray(data);
    out.data = reinterpret_cast<U*>(values->mutable_data());
    return out;
}


static std::optional<uint64_t> get_file_size(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    return static_cast<uint64_t>(sz);
}

template <typename Payload>
static int64_t count_records_truncating(const fs::path& path) {
    auto sz_opt = get_file_size(path);
    if (!sz_opt) return 0;
    const uint64_t sz = *sz_opt;
    const uint64_t rec = sizeof(Payload);
    return static_cast<int64_t>(sz / rec);
}

inline std::string part_name(int part_idx) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "part-%05d.parquet", part_idx);
    return std::string(buf);
}
