#include "simulator.hpp"
#include "pcg32.hpp"

#include <boost/asio.hpp>
#include <iostream>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include "logging.hpp"

int main() {
    try {
        auto core = boost::log::core::get();
        core->set_filter(
            boost::log::expressions::attr<LogLevel>("Severity") >= LogLevel::LL_DEBUG
        );
        boost::asio::io_context io_context;

        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);

        // Resolve localhost:16000
        auto endpoints = resolver.resolve("127.0.0.1", "16000");

        // Connect to the exchange
        boost::asio::connect(socket, endpoints);

        const std::array<Price_t, 3> bounds = {1, 5, 10};

        MarketSimulator<3> sim(
            io_context,
            std::move(socket),
            std::make_unique<PCGRNG>(0, 0),
            bounds
        );

        sim.start();

        io_context.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Simulator error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
