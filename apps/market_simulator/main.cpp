#include "simulator.hpp"
#include "pcg32.hpp"

#include <boost/asio.hpp>
#include <iostream>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include "logging.hpp"
#include <thread>
#include <vector>

int main() {
    try {
        size_t n_simulators = 5;

        // Setup logging filter
        auto core = boost::log::core::get();
        core->set_filter(
            boost::log::expressions::attr<LogLevel>("Severity") >= LogLevel::LL_WARNING
        );

        // Bounds for the simulator
        const std::array<Price_t, 3> bounds = {1, 5, 10};

        // Threads to hold simulators
        std::vector<std::thread> threads;
        threads.reserve(n_simulators);

        for (size_t i = 0; i < n_simulators; ++i) {
            threads.emplace_back([i, bounds]() {
                try {
                    boost::asio::io_context io_context;

                    // Resolve and connect to exchange
                    tcp::resolver resolver(io_context);
                    tcp::socket socket(io_context);
                    auto endpoints = resolver.resolve("127.0.0.1", "16000");
                    boost::asio::connect(socket, endpoints);

                    // Each simulator gets its own RNG seed
                    auto rng = std::make_unique<PCGRNG>(static_cast<uint64_t>(i), 0);

                    MarketSimulator<3> sim(
                        io_context,
                        std::move(socket),
                        std::move(rng),
                        bounds
                    );

                    sim.start();
                    io_context.run();
                } catch (const std::exception& e) {
                    std::cerr << "[Simulator " << i << "] Error: " << e.what() << "\n";
                }
            });
        }

        // Wait for all simulators to finish (they likely run indefinitely)
        for (auto& t : threads) {
            t.join();
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Simulator error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
