#pragma once
#include <memory>
#include <vector>
#include <cmath>
#include <vector>
#include <algorithm>

#define EPS 1.0e-08

class RNG {
    public:
        virtual ~RNG() = default;

        virtual void seed(uint64_t seed, uint64_t stream) = 0;
        virtual std::unique_ptr<RNG> clone() const = 0;

        virtual double standard_uniform() = 0;
        virtual double standard_normal() = 0;
        virtual double exponential(double lambda) = 0;

        virtual bool bernoulli(double p) = 0;
		virtual uint32_t uniform_int(uint32_t lower_bound, uint32_t upper_bound) = 0;
		virtual uint32_t poisson(double mean) = 0;
        virtual size_t categorical(const std::vector<double>& cumulative_probs) = 0;

        virtual void normal_vector(std::vector<double>& out) = 0;
};

inline double normal_density(const double x) {return x < -10.0 || 10.0 < x ? 0.0 : exp(-0.5 * x * x) / 2.506628274631;} 

inline double normal_cdf(const double x) {
	if (x < -10.0) return 0.0;
	if (x > 10.0) return 1.0;
    if (x < 0.0) return 1.0 - normal_cdf(-x);

	static constexpr double p = 0.2316419;
	static constexpr double b1 = 0.319381530;
	static constexpr double b2 = -0.356563782;
	static constexpr double b3 = 1.781477937;
	static constexpr double b4 = -1.821255978;
	static constexpr double b5 = 1.330274429;
	const auto t = 1.0 / (1.0 + p*x);
    const auto pol = t*(b1 + t*(b2 + t*(b3 + t*(b4 + t*b5))));
    const auto pdf = normal_density(x);
	return 1.0 - pdf * pol;
}

inline double inverse_normal_cdf(const double p) {
    const bool sup = p > 0.5;
    const double up = sup ? 1.0 - p : p;
	static constexpr double a0 = 2.50662823884;
	static constexpr double a1 = -18.61500062529;
	static constexpr double a2 = 41.39119773534;
	static constexpr double a3 = -25.44106049637;
	static constexpr double b0 = -8.47351093090;
	static constexpr double b1 = 23.08336743743;
	static constexpr double b2 = -21.06224101826;
	static constexpr double b3 = 3.13082909833;
	static constexpr double c0 = 0.3374754822726147;
	static constexpr double c1 = 0.9761690190917186;
	static constexpr double c2 = 0.1607979714918209;
	static constexpr double c3 = 0.0276438810333863;
	static constexpr double c4 = 0.0038405729373609;
	static constexpr double c5 = 0.0003951896511919;
	static constexpr double c6 = 0.0000321767881768;
	static constexpr double c7 = 0.0000002888167364;
	static constexpr double c8 = 0.0000003960315187;
	double x = up - 0.5;
	double r;

	if (std::fabs(x) < 0.42) {
		r = x * x;
		r = x * (((a3 * r + a2) * r + a1) * r + a0) / ((((b3 * r + b2) * r + b1) * r + b0) * r + 1.0);
		return sup ? -r : r;
	}

	r = up;
	r = std::log(-std::log(r));
	r = c0 + r * (c1 + r * (c2 + r * (c3 + r * (c4 + r * (c5 + r * (c6 + r * (c7 + r * c8)))))));

	return sup ? r: -r;
}