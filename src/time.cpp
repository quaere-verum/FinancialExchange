#pragma once
#include "time.hpp"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

Time_t utc_now_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);

    Time_t t = (Time_t(ft.dwHighDateTime) << 32) | Time_t(ft.dwLowDateTime);

    return t * 100ULL;
}

#elif defined(__APPLE__) || defined(__linux__)

#include <time.h>

Time_t utc_now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    return Time_t(ts.tv_sec) * 1'000'000'000ULL + Time_t(ts.tv_nsec);
}

#else
#error "Unsupported platform"
#endif
