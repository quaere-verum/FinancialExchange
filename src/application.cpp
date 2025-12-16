#include "application.hpp"
#include <iostream>

Application::Application(uint16_t port, size_t num_threads)
    : io_context_(),
    signals_(io_context_, SIGINT, SIGTERM),
    port_(port) {
    work_guard_.emplace(io_context_.get_executor());
    exchange_ = std::make_unique<Exchange>(io_context_, port);
    threads_.reserve(num_threads);
    signals_.async_wait(
        [this](const boost::system::error_code&, int) {
            this->stop();
        }
    );
}

void Application::start() {
    if (running_.exchange(true)) {return;}
    exchange_->start();
    for (size_t i = 0; i < threads_.capacity(); ++i) {
        threads_.emplace_back([this]() {
            run_io_context();
        });
    }
    std::cout << "Exchange started. Listening on port " << port_ << ", using " << threads_.size() << " threads.\n";
}

void Application::run_io_context() {
    try {
        io_context_.run();
    } catch (const std::exception& e) {
        std::terminate();
    }
}

void Application::stop() {
    if (!running_.exchange(false)) {return;}

    exchange_->stop();
    work_guard_.reset();
    io_context_.stop();

    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }

    threads_.clear();
}

void Application::wait() {
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
}