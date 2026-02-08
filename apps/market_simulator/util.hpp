#pragma once
#include "types.hpp"
#include "rng.hpp"

enum class AgentType : uint8_t {
    MAKER = 0,
    TAKER = 1,
    DEEP = 2,
    NOISE = 3,
    META = 4
};

enum class OrderRegime : uint8_t {
    MARKETABLE = 0,
    IMPROVE = 1,
    PASSIVE = 2
};

struct InsertDecision {
    Side side;
    Price_t price;
    Volume_t quantity;
    Lifespan lifespan;
    double cancellation_hazard_mass; // cancellation threshold mass (larger => longer survival under same hazard accumulation)
};

struct AgentMixState {
    AgentType last = AgentType::NOISE;
    double mm_boost = 0.0;
    double taker_boost = 0.0;
    double deep_boost = 0.0;
    double noise_boost = 0.0;
};

inline int sample_geometric_offset(RNG* rng, double p, int cap) {
    p = std::clamp(p, 0.05, 0.95);
    int k = 0;
    while (k < cap && rng->standard_uniform() > p) ++k;
    return k;
}

static inline Price_t stochastic_round(double x, RNG* rng) {
    double f = std::floor(x);
    double frac = x - f;
    return static_cast<Price_t>(f + (rng->standard_uniform() < frac ? 1.0 : 0.0));
}