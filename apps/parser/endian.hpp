#pragma once
#include "types.hpp"
#include "util.hpp"
#include <cstdint>
#if defined(_MSC_VER)
  #include <intrin.h>
#endif

static inline bool host_is_little_endian() {
    const uint16_t x = 0x0102;
    return *reinterpret_cast<const uint8_t*>(&x) == 0x02;
}

template <typename T>
static inline T bswap(T v) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
                  "bswap only supports integral/enum types");

    using U = underlying_or_self_t<T>;
    static_assert(std::is_integral_v<U>, "Underlying type must be integral");

    U u{};
    std::memcpy(&u, &v, sizeof(U));

    if constexpr (sizeof(U) == 1) {
        return v;
    } else if constexpr (sizeof(U) == 2) {
#if defined(_MSC_VER)
        const auto rr = _byteswap_ushort(static_cast<unsigned short>(u));
        U r = static_cast<U>(rr);
#else
        U r = static_cast<U>(__builtin_bswap16(static_cast<uint16_t>(u)));
#endif
        T out{};
        std::memcpy(&out, &r, sizeof(U));
        return out;

    } else if constexpr (sizeof(U) == 4) {
#if defined(_MSC_VER)
        const auto rr = _byteswap_ulong(static_cast<unsigned long>(u));
        U r = static_cast<U>(rr);
#else
        U r = static_cast<U>(__builtin_bswap32(static_cast<uint32_t>(u)));
#endif
        T out{};
        std::memcpy(&out, &r, sizeof(U));
        return out;

    } else if constexpr (sizeof(U) == 8) {
#if defined(_MSC_VER)
        const auto rr = _byteswap_uint64(static_cast<unsigned long long>(u));
        U r = static_cast<U>(rr);
#else
        U r = static_cast<U>(__builtin_bswap64(static_cast<uint64_t>(u)));
#endif
        T out{};
        std::memcpy(&out, &r, sizeof(U));
        return out;

    } else {
        static_assert(sizeof(U) == 1 || sizeof(U) == 2 || sizeof(U) == 4 || sizeof(U) == 8,
                      "Unsupported integer size for bswap");
        return v; 
    }
}

template <typename T>
static inline T from_be(T v_be) {
    if (host_is_little_endian()) {
        return bswap(v_be);
    }
    return v_be;
}