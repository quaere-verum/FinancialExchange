#pragma once

#include <ostream>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <boost/log/keywords/channel.hpp>
#include <boost/log/sources/global_logger_storage.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_channel_logger.hpp>


enum class LogLevel : unsigned char {
    LL_DEBUG,
    LL_INFO,
    LL_WARNING,
    LL_ERROR,
    LL_FATAL
};

constexpr const char* LOG_LEVEL_NAMES[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "FATAL"
};

template<typename C, typename T>
std::basic_ostream<C, T>& operator<<(std::basic_ostream<C, T>& strm, LogLevel lvl) {
    auto levelNumber = static_cast<int>(lvl);
    strm << LOG_LEVEL_NAMES[levelNumber];
    return strm;
}

#define TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(loggerName, channelName)\
    BOOST_LOG_INLINE_GLOBAL_LOGGER_CTOR_ARGS(loggerName,\
        boost::log::sources::severity_channel_logger<LogLevel>,\
        (boost::log::keywords::channel = (channelName)));

#define RLOG(loggerName, logLevel) BOOST_LOG_SEV(loggerName::get(), (logLevel))


class CSVLogger {
    public:
        struct Record {
            Time_t timestamp_ns;
            std::string event;
            Id_t id;
            bool side;
            Price_t price;
            Volume_t quantity;
        };

        explicit CSVLogger(const std::string& file_path, size_t batch_size = 1024)
        : batch_size_(batch_size) {
            file_.open(file_path, std::ios::out | std::ios::trunc);
            file_ << "timestamp_ns,event,id,side,price,quantity\n";

            writer_ = std::thread(&CSVLogger::writer_loop, this);
        };

        ~CSVLogger() {
            stop();
        };

        CSVLogger(const CSVLogger&) = delete;
        CSVLogger& operator=(const CSVLogger&) = delete;

        void log(Record&& record) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push(std::move(record));
            }
            cv_.notify_one();
        };
        void stop() {
            bool expected = true;
            if (running_.compare_exchange_strong(expected, false)) {
                cv_.notify_all();
                if (writer_.joinable())
                    writer_.join();
                file_.flush();
                file_.close();
            }
        };

    private:
        void writer_loop() {
            std::vector<Record> batch;
            batch.reserve(batch_size_);

            while (running_ || !queue_.empty()) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&] {
                        return !running_ || !queue_.empty();
                    });

                    while (!queue_.empty() && batch.size() < batch_size_) {
                        batch.push_back(std::move(queue_.front()));
                        queue_.pop();
                    }
                }

                if (!batch.empty()) {
                    write_batch(batch);
                    batch.clear();
                }
            }
        };
        void write_batch(std::vector<Record>& batch) {
            for (const auto& r : batch) {
                file_
                    << r.timestamp_ns << ','
                    << r.event << ','
                    << r.id << ','
                    << r.side << ','
                    << r.price << ','
                    << r.quantity << '\n';
            }

            file_.flush();
        };

        std::ofstream file_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<Record> queue_;
        std::thread writer_;
        std::atomic<bool> running_{true};
        const size_t batch_size_;
};
