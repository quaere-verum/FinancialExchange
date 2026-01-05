#include "simulator.hpp"
#include "pcg32.hpp"

#include <boost/asio.hpp>
#include <iostream>

int main() {
    try {
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

        sim.set_amend_intensity(30.0);
        sim.set_cancel_intensity(30.0);
        sim.set_insert_intensity(10.0);

        sim.start();

        io_context.run();
    }
    catch (const std::exception& e) {
        std::cerr << "Simulator error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
