#include "application.hpp"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include "logging.hpp"

int main(int argc, char* argv[]) {
    try {
        auto core = boost::log::core::get();
        core->set_filter(
            boost::log::expressions::attr<LogLevel>("Severity") >= LogLevel::LL_DEBUG
        );
        uint16_t port = 16000;
        std::size_t io_threads = 5;

        if (argc > 1) {
            int p = std::atoi(argv[1]);
            if (p > 0 && p <= 65535) {
                port = static_cast<uint16_t>(p);
            } else {
                std::cerr << "Invalid port number, using default: " << port << "\n";
            }
        }

        if (argc > 2) {
            int t = std::atoi(argv[2]);
            if (t > 0) {
                io_threads = static_cast<std::size_t>(t);
            } else {
                std::cerr << "Invalid thread count, using default: " << io_threads << "\n";
            }
        }

        Application app(port, io_threads);
        app.start();
        app.wait();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}
