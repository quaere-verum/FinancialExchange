#pragma once
#include "state.hpp"
#include "util.hpp"
#include "market_maker.hpp"
#include "taker.hpp"
#include "deep_trader.hpp"
#include "noise_trader.hpp"
#include "meta_trader.hpp"
#include "parameters.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <iostream>



class MarketDynamics {
public:
    MarketDynamics(
        MarketMakerParams mm_params = {}, 
        TakerParams taker_params = {},
        DeepParams deep_params = {},
        NoiseParams noise_params = {},
        MetaParams meta_params = {}
    ) 
    : mm_archetype_(mm_params)
    , taker_archetype_(taker_params)
    , meta_archetype_(meta_params)
    , deep_params_(std::move(deep_params))
    , noise_params_(std::move(noise_params)) {}

    void decide_insert(
        const FeatureVector& state, 
        double cumulative_hazard, 
        size_t batch_size, 
        RNG* rng,
        std::vector<InsertDecision>& insert_decisions
    ) {
        AgentType agent_type = sample_agent(rng);

        constexpr double BUMP_BASE = 0.35;
        // 0 below 0, 1 above 1
        const double gate = 1.0 / (1.0 + std::exp(-4.0 * (2.0 * state.toxicity01 - 1.0)));
        const double bump = (1.0 - gate) * BUMP_BASE / static_cast<double>(std::max((size_t)1, batch_size));
        switch (agent_type) {
            case AgentType::MAKER: agent_mix_state_.mm_boost    += bump; break;
            case AgentType::TAKER: agent_mix_state_.taker_boost += bump; break;
            case AgentType::DEEP:  agent_mix_state_.deep_boost  += bump; break;
            case AgentType::NOISE: agent_mix_state_.noise_boost += bump; break;
            case AgentType::META:  break;
        }

        agent_mix_state_.mm_boost    = std::clamp(agent_mix_state_.mm_boost,    -0.5, 0.5);
        agent_mix_state_.taker_boost = std::clamp(agent_mix_state_.taker_boost, -0.5, 0.5);
        agent_mix_state_.deep_boost  = std::clamp(agent_mix_state_.deep_boost,  -0.5, 0.5);
        agent_mix_state_.noise_boost = std::clamp(agent_mix_state_.noise_boost, -0.5, 0.5);
        if (agent_type != AgentType::META) agent_mix_state_.last = agent_type;
        
        switch (agent_type) {
            case AgentType::MAKER: mm_archetype_.decide_insert(state, cumulative_hazard, rng, insert_decisions); break;
            case AgentType::TAKER: taker_archetype_.decide_insert(state, cumulative_hazard, rng, insert_decisions); break;
            case AgentType::DEEP: decide_deep_insert(state, cumulative_hazard, rng, deep_params_, insert_decisions); break;
            case AgentType::NOISE: decide_noise_insert(state, cumulative_hazard, rng, noise_params_, insert_decisions); break;
            case AgentType::META: meta_archetype_.decide_insert(state, cumulative_hazard, rng, insert_decisions); break;
            default: throw std::runtime_error("Unknown AgentType.");
        }

        return;
    }

    void update_intensity(
        const FeatureVector& state,
        size_t open_order_count,
        double& lambda_insert,
        double& lambda_cancel
    ) const {
        const double dt = std::max(1e-9, (double)state.time_since_previous_sync);
        // META activity in [0,1] (drives both activity and churn)
        const double meta = std::clamp((double)meta_archetype_.activity(), 0.0, 1.0);

        // Open-order feedback control in log space (stable across magnitudes)
        constexpr double OPEN_TARGET = 1'000.0;
        const double open_log = std::log1p((double)open_order_count);
        const double open_target_log = std::log1p(OPEN_TARGET);
        const double open_err = open_log - open_target_log; // >0 means too many open orders

        // ---- insert intensity target (log-linear; yields clustering & state dependence) ----
        // Base level
        double log_li = std::log(LAMBDA_INSERT_BASE);

        // Drivers (tuned to be moderate; excite does most of the clustering work)
        log_li += 0.40 * state.stress01;                            // stress => more updates/flow
        log_li += 0.25 * state.trade_rate01;                        // tape accelerating => more events
        log_li += 0.18 * state.thin01;                              // thin book => more refill
        log_li += 0.06 * state.spread01;                            // wide spread => some added activity
        log_li += 0.55 * std::fabs(state.flow_imbalance);           // directional pressure => more events

        // Excitation is the main burst mechanism
        log_li += 0.55 * state.excitement01;

        // Meta boosts event flow
        log_li += 0.90 * meta;

        // Penalize inserts when open orders are already too high
        log_li -= 0.80 * std::max(0.0, open_err);

        // Clamp target
        const double li_target = std::clamp(std::exp(log_li), 500.0, 900'000.0);

        // ---- cancel intensity target ----
        // Structure: cancellations scale with open orders, modulated by stress/excite/meta
        // Start from the baseline ratio implied by your constants:
        // at OPEN_TARGET, want lambda_cancel ≈ LAMBDA_CANCEL_BASE.
        const double k1 = LAMBDA_CANCEL_BASE / 10.0;
        const double k0 = LAMBDA_CANCEL_BASE / std::max(1.0, OPEN_TARGET); // cancels/sec per open order at baseline
        double log_lc = std::log(k0 * std::max(1.0, (double)open_order_count));

        // Drivers: stress/excite increase churn; thinness decreases cancels (avoid draining when already thin)
        log_lc += 0.75 * state.stress01;
        log_lc += 0.25 * state.trade_rate01;
        log_lc += 0.85 * state.excitement01;     // strong: cancels cluster strongly in bursts
        log_lc += 0.60 * meta;

        // If book is too big vs target, cancel more; if too small, cancel less.
        log_lc += 1.00 * std::max(0.0, open_err);
        log_lc -= 0.35 * std::max(0.0, -open_err);

        // Avoid draining a thin book during stress: makers often pull back, but if depth is already low,
        // too much cancel will create unrealistic empty-book behavior.
        log_lc -= 0.25 * state.thin01;

        // Clamp target
        const double lc_target = std::clamp(std::exp(log_lc), 500.0, 1'200'000.0);

        // ---- smoothing / mean reversion (prevents twitchy rates) ----
        // Use different taus: cancels react faster than inserts in many venues (cancel/replace).
        constexpr double TAU_INS = 0.12; // seconds
        constexpr double TAU_CAN = 0.07; // seconds

        const double aI = 1.0 - std::exp(-dt / TAU_INS);
        const double aC = 1.0 - std::exp(-dt / TAU_CAN);

        lambda_insert = (1.0 - aI) * lambda_insert + aI * li_target;
        lambda_cancel = (1.0 - aC) * lambda_cancel + aC * lc_target;

        // ---- safety: keep within plausible bounds ----
        lambda_insert = std::clamp(lambda_insert, 50.0, 1'500'000.0);
        lambda_cancel = std::clamp(lambda_cancel, 50.0, 2'000'000.0);
    }


    void sync_with_state(const FeatureVector& state, double lambda_insert, RNG* rng) {
        meta_archetype_.update(state, rng);
        update_agent_mix(state, rng);
        set_agent_weights(state, lambda_insert);
        mm_archetype_.update(state);
        taker_archetype_.update(state);
    }

private:
    inline void set_agent_weights(
        const FeatureVector& state,
        double lambda_insert
    ) {
        const double lambda_ratio = std::clamp(
            std::log((lambda_insert + 1e-6) / (LAMBDA_INSERT_BASE + 1e-6)),
            -1.5, 1.5
        );
        // 0 below 0, 1 above 1
        const double mm_gate = 1.0 / (1.0 + std::exp(-4.0 * (state.toxicity01 - 1.0)));

        // Interpret: higher logit => more likely
        double l_mm =
            2.50
            - 0.6 * state.spread01        // tight spread => MM more likely
            + 0.2 * state.trade_rate01
            - 3.0 * mm_gate
            - 0.40 * std::max(0.0, lambda_ratio);

        double l_taker =
            -0.80
            + 0.25 * std::fabs(state.flow_imbalance)
            + 0.15 * state.trade_rate01
            + 0.25 * state.stress01 
            - 0.35 * state.spread01
            + 0.24 * state.stress01 * state.spread01 // only in stressed markets does wide spread go with urgency
            + 0.2 * std::max(0.0, lambda_ratio);

        double l_deep =
            -0.10
            + 0.6 * state.stress01
            + 0.4 * state.spread01
            - 0.2 * state.trade_rate01         // in very active tape, deep provision relatively less
            - 0.15 * lambda_ratio;

        double l_noise =
            0.10
            + 0.1 * state.trade_rate01
            - 0.1 * state.stress01       // noise always there
            - 0.25 * lambda_ratio;

        double l_meta = -4.0 + 6.0 * meta_archetype_.activity(); // essentially off unless episode model turns it on

        // Inertia: stickiness to last selected type
        constexpr double INERTIA_BONUS = 0.2;
        switch (agent_mix_state_.last) {
            case AgentType::MAKER: l_mm    += INERTIA_BONUS + agent_mix_state_.mm_boost; break;
            case AgentType::TAKER: l_taker += INERTIA_BONUS + agent_mix_state_.taker_boost; break;
            case AgentType::DEEP:  l_deep  += INERTIA_BONUS + agent_mix_state_.deep_boost; break;
            case AgentType::NOISE: l_noise += INERTIA_BONUS + agent_mix_state_.noise_boost; break;
            case AgentType::META: break;
        }

        l_mm   += 0.3 * state.thin01 * (1.0 - 1.5 * mm_gate);
        l_deep += 0.2 * state.thin01;

        // Softmax sample
        const std::array<double, 5> L = {l_mm, l_taker, l_deep, l_noise, l_meta};
        double m = *std::max_element(L.begin(), L.end());
        double Z = 0.0;
        for (size_t i = 0; i < 5; ++i) { agent_cdf_[i] = std::exp(L[i] - m); Z += agent_cdf_[i]; }
        const double invZ = 1.0 / Z;
        double c = 0.0;
        for (size_t i = 0; i < 5; ++i) {
            c += agent_cdf_[i] * invZ;
            agent_cdf_[i] = c;
        }
        agent_cdf_.back() = 1.0;

        return;
    }

    void update_agent_mix(const FeatureVector& state, RNG* rng) {
        const double gate = 1.0 / (1.0 + std::exp(-4.0 * (state.toxicity01 - 1.0)));
        constexpr double TAU_INERTIA_CALM = 0.2;
        constexpr double TAU_INERTIA_STRESS = 0.01;

        const double tau_inertia = TAU_INERTIA_CALM * (1.0 - 0.8 * gate) + TAU_INERTIA_STRESS * gate;

        const double dt = state.time_since_previous_sync;
        const double a = 1.0 - std::exp(-dt / tau_inertia);

        agent_mix_state_.mm_boost *= (1.0 - a);
        agent_mix_state_.taker_boost *= (1.0 - a);
        agent_mix_state_.deep_boost *= (1.0 - a);
        agent_mix_state_.noise_boost *= (1.0 - a);

    }

    inline AgentType sample_agent(RNG* rng) {
        // double u = rng->standard_uniform();
        // for (size_t i = 0; i < agent_cdf_.size(); ++i) {
        //     if (u <= agent_cdf_[i]) return static_cast<AgentType>(i);
        // }
        // return static_cast<AgentType>(agent_cdf_.size() - 1);
        // MAKER = 0,
        // TAKER = 1,
        // DEEP = 2,
        // NOISE = 3,
        // META = 4
        const double u = rng->standard_uniform();
        const double denom = 
            agent_cdf_[0] 
            + agent_cdf_[1] 
            // + agent_cdf_[2] 
            + 1e-6;
        const double p_maker = agent_cdf_[0] / denom;
        const double p_taker = agent_cdf_[0] / denom;
        if (u <= p_maker) {
            return AgentType::MAKER;
        } else {
            return AgentType::TAKER;
        }
        // } else if (u <= p_maker + p_taker) {
        //     return AgentType::TAKER;
        // } else {
        //     return AgentType::DEEP;
        // }
    }


    private:
        AgentMixState agent_mix_state_;
        MarketMakerAgent mm_archetype_;
        TakerAgent taker_archetype_;
        MetaAgent meta_archetype_;
        DeepParams deep_params_;
        NoiseParams noise_params_;
        
        std::array<double, 5> agent_cdf_ = {0.65, 0.75, 0.85, 0.95, 1.0};
};
