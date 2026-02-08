#pragma once
#include "types.hpp"

// Arrival process intensities
constexpr double LAMBDA_TOTAL = 25'000;
constexpr double CANCEL_FRACTION = 0.70;
constexpr double LAMBDA_INSERT_BASE = LAMBDA_TOTAL * (1 - CANCEL_FRACTION);
constexpr double LAMBDA_CANCEL_BASE = LAMBDA_TOTAL * CANCEL_FRACTION;

// Initial values
constexpr double INITIAL_FAIR_VALUE = 1'000.0;
constexpr double INITIAL_LOG_FAIR_VALUE = 6.907755278982137; // log(1000)

// Parameters for derived features
constexpr double SPREAD_CLIP_RANGE = 10.0;
constexpr double SPREAD_IF_ONESIDED = 10.0;
constexpr double DEPTH_TIME_SCALE = 0.5;
constexpr double TRADE_RATE_SCALE = 0.5;
constexpr double JUMP_INTENSITY_SCALE = 1.0;
constexpr double GAP_IN_SPREAD_SCALE = 2.0;
constexpr double W_DIR_TO_TOX = 0.45;
constexpr double W_GAP_TO_TOX = 0.35;
constexpr double W_SURP_TO_TOX = 0.20;
constexpr double SURPRISE_LOG_SCALE = 0.70;
constexpr double W_SPREAD_TO_STRESS = 0.15;
constexpr double W_VOL_TO_STRESS = 0.40;
constexpr double W_THIN_TO_STRESS = 0.20;
constexpr double W_EXC_TO_STRESS = 0.25;
constexpr double TERM_GAMMA = 1.8;
constexpr double W_THIN_TO_STRESS_MICRO = 0.35;
constexpr double W_VOL_TO_STRESS_MICRO = 0.45;
constexpr double W_EXC_TO_STRESS_MICRO = 0.20; 
constexpr double W_TOX_TO_STRESS_MICRO = 0.35; 
constexpr double W_SPREAD_TO_STRESS_MICRO = 0.10;
constexpr double MICRO_GAMMA = 3.0;


struct TakerParams {
    // Latent dynamics
    double tau_urgency = 0.30;   // seconds (fast)
    double tau_bias    = 0.80;   // seconds (slower)

    // How state features map into urgency/bias targets
    double w_flow_abs  = 0.65;   // |flow_imbalance| -> urgency
    double w_surprise  = 0.25;   // volume_surprise -> urgency
    double w_stress    = 0.35;   // vol_stress_pos  -> urgency
    double w_jump      = 0.15;   // jump_intensity  -> urgency (optional)

    double w_flow_sign = 0.90;   // signed flow -> bias target
    double w_fair      = 0.10;   // fair-mid gap -> bias target (weak)

    // Aggressiveness model
    double p_cross_base = 0.05;   // baseline chance to cross spread
    double p_cross_max  = 0.70;   // at urgency=1 and tight spreads
    double p_join_best_floor = 0.20; // minimum join-best probability

    // Sizes
    Volume_t min_qty = 1;
    Volume_t max_qty = 200;
    double mean_qty  = 6.0;
    double qty_sigma = 0.9;      // lognormal tail
    double urgency_size_boost = 1.2; // urgency increases size

    // For taker-like flow, cancels should be relatively *high* when not urgent,
    // and *low* when urgent (they want execution).
    double hazard_min = 0.05;
    double hazard_max = 1.50;
};

struct NoiseParams {
    // Noise places mostly near-touch but often non-marketable.
    // Offsets are in ticks from best on that side.
    int max_offset_ticks = 6;

    // Probability of being marketable (small, increases when spread is tight)
    double p_marketable_base = 0.01;
    double p_marketable_max  = 0.10;

    // Sizes (small)
    Volume_t min_qty = 1;
    Volume_t max_qty = 30;
    double mean_qty = 3.0;
    double qty_sigma = 0.7;

    // Noise cancels quickly / is flaky
    double hazard_mass = 0.9;
    double lifespan = 0.15;

    // Side is near-uniform; can add tiny drift from flow
    double flow_follow = 0.10; // 0..1, small
};

struct MetaParams {
    // Episode arrival model (hazard per second, then modulated by stress/rate)
    double base_hazard_interval = 180.0; // mean time between episodes in calm markets
    double hazard_stress_w = 0.7;        // increases start hazard with stress
    double hazard_rate_w   = 0.4;        // increases start hazard with tape speed
    double start_prob_cap  = 0.25;       // cap per-tick start probability

    // Episode duration distribution
    double mean_episode_time = 1.0;      // seconds, exponential

    // Activity mapping: how strongly META dominates when active
    double activity_when_active = 1.0;   // [0..1] (you can set <1 to soften dominance)
    double activity_decay_tau = 0.12;    // seconds: smooth activity transitions a bit

    // Placement aggressiveness
    int max_sweep_ticks = 60;
    int base_sweep_cap  = 12;

    double p_geom_calm   = 0.70;  // shallow tail
    double p_geom_urgent = 0.30;  // fatter tail (smaller p)

    // If spread is huge and urgency not extreme, sometimes post passively to reduce cost.
    double wide_spread_ticks = 6.0;
    double p_passive_when_wide = 0.65;

    // Size model
    Volume_t min_qty = 1;
    Volume_t max_qty = 5000;
    double mean_qty  = 60.0;
    double qty_sigma = 1.1;

    double urgency_size_boost = 3.0; // mean multiplier: (1 + boost * urgency)
    double surprise_size_boost = 0.8;

    // Cancel/lifespan
    double hazard_min = 0.02;
    double hazard_max = 0.50;

    // Side selection at episode start
    double side_flow_w = 0.35; // p_buy = clamp(0.5 + w*flow, ...)
    double side_pmin   = 0.10;
    double side_pmax   = 0.90;

    // Optional: reduce sweep cap when top-of-book is thin
    double thin_cap_reduction = 0.4; // multiply cap by (1 - thin_cap_reduction*thin01)
};

constexpr size_t MM_LADDER_SIZE = 6;

struct MarketMakerParams {
    // --- Quoting style ---
    double p_improve_when_wide = 0.65;
    double p_one_sided_when_stressed = 0.15;

    // NEW: touch participation (join/backoff)
    double p_join_touch_base = 0.92;        // calm: usually join touch
    double join_risk_slope   = 0.75;        // risk reduces join prob
    double improve_risk_slope = 0.70;       // risk reduces improve prob
    Price_t backoff_ticks_max = 4;          // in stress, quote up to this far away (at lvl0)

    // NEW: stochastic ladder depth
    size_t ladder_levels_min = 1;           // per side (when quoting that side)
    size_t ladder_levels_max = MM_LADDER_SIZE / 2; // per side
    double ladder_level_mean_calm = 2.5;    // expected levels per side when risk~0
    double ladder_level_mean_stress = 1.2;  // expected levels per side when risk~1

    // --- Size model ---
    Volume_t min_qty = 1;
    Volume_t max_qty = 50;
    double mean_qty = 5.0;
    double qty_logn_sigma = 0.6;

    // --- Stress response ---
    double stress_spread_strength = 0.7;
    double stress_vol_strength = 0.9;
    double stress_flow_strength = 0.6;

    // --- Cancellation / hazard (NOTE: lower mass => faster cancel in your model) ---
    double base_hazard_mass = 1.0;
    double hazard_mass_min = 0.15;
    double hazard_mass_max = 4.0;

    // NEW: stress should DECREASE hazard mass (faster cancels)
    double hazard_stress_strength = 1.0;    // risk reduces mass by up to this fraction
    double hazard_thin_strength   = 0.5;    // thin reduces mass further

    // NEW: level shaping (touch smaller mass than deep)
    double hazard_level_slope = 0.25;       // each deeper level increases mass
};

struct DeepParams {
    // Deep placement region (ticks from best)
    int min_offset_ticks = 5;    // do not clutter near-touch
    int max_offset_ticks = 40;

    // Tail: higher stress => smaller p => deeper tail (more far quotes)
    double p_base = 0.75;        // geometric p when calm
    double p_stress_slope = 0.35;

    // Sizes
    Volume_t min_qty = 1;
    Volume_t max_qty = 800;
    double mean_qty = 20.0;
    double qty_sigma = 1.0;

    // Cancellation behavior: deep liquidity is “sticky”
    double hazard_mass = 0.15;   // low hazard mass => long-lived]

    // Mild contrarian to flow (deep providers refill the pressured side)
    double flow_contrarian = 0.25; // 0..1
};