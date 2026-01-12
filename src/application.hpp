#pragma once

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <optional>

#include "exchange.hpp"

class Application {
    public:
        explicit Application(uint16_t port, size_t num_threads = 1, std::string log_file = "logs.csv");

        void start();
        void stop();
        void wait();

    private:
        void run_io_context();

        boost::asio::io_context io_context_;
        using work_guard_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

        std::optional<work_guard_t> work_guard_;

        std::unique_ptr<Exchange> exchange_;
        std::vector<std::thread> threads_;
        std::atomic<bool> running_{false};
        boost::asio::signal_set signals_;
        uint16_t port_;
};
