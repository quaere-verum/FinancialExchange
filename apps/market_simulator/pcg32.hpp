#pragma once
#include "rng.hpp"
#include <random>

#include <cstdint>
#include <cmath>
#include <cassert>
#include <numeric>

class pcg32 {
    public:
        using result_type = uint32_t;
        pcg32(uint64_t seed = 0, uint64_t stream = 1) {seed_rng(seed, stream);}

        void seed_rng(uint64_t seed, uint64_t stream = 1) {
            state_ = 0;
            inc_ = (stream << 1u) | 1u;
            next_uint();
            state_ += seed;
            next_uint();
        }

        result_type next_uint() {
            uint64_t oldstate = state_;
            state_ = oldstate * multiplier_ + inc_;

            uint32_t xorshifted = static_cast<uint32_t>(
                ((oldstate >> 18u) ^ oldstate) >> 27u
            );
            uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);

            return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
        }

        uint32_t operator()() {
			return next_uint();
		}

		static constexpr uint32_t min() {
			return 0;
		}

		static constexpr uint32_t max() {
			return UINT32_MAX;
		}

        inline double uniform() {
            // Convert to (0,1) with 32-bit precision
            return (next_uint() + 0.5) * inv_uint32_;
        }

    private:
        uint64_t state_ = 0;
        uint64_t inc_ = 0;

        static constexpr uint64_t multiplier_ = 6364136223846793005ULL;
        static constexpr double   inv_uint32_ = 1.0 / 4294967296.0;
};


class PCGRNG final : public RNG {
    public:
        explicit PCGRNG(uint64_t seed, uint64_t stream) : rng_(seed, stream) {};

        void seed(uint64_t seed, uint64_t stream) override {rng_.seed_rng(seed, stream);};
        std::unique_ptr<RNG> clone() const override {return std::make_unique<PCGRNG>(*this);};

        inline double standard_uniform() override {return rng_.uniform();}

        inline double standard_normal() override {
            double p = rng_.uniform();
            return inverse_normal_cdf(p);
        }
        
        inline double exponential(double lambda) override {
            double p = rng_.uniform();
            return -std::log(1 - p) / lambda; // inverse cdf for exponential is -ln(1 - p) / lambda
        }
        
        inline bool bernoulli(double p) override {
            return rng_.uniform() > 0.5;
        }
        
        inline uint32_t uniform_int(uint32_t lower_bound, uint32_t upper_bound) {
            std::uniform_int_distribution<uint32_t> dist(lower_bound, upper_bound);
            return dist(rng_);
        }

        inline size_t categorical(const std::vector<double>& cumulative_probs) override {
            // Linear scan because nr of categories assumed to be small (<10)
            assert(cumulative_probs.back() > 0.999999);
            double u = rng_.uniform();

            for (size_t i = 0; i < cumulative_probs.size(); ++i) {
                if (u < cumulative_probs[i]) {
                    return i;
                }
            }
            return cumulative_probs.size() - 1;
        }

        inline void normal_vector(std::vector<double>& out) override {
            for (auto x : out) {
                x = standard_normal();
            }
        };

    private:
        pcg32 rng_;
};
