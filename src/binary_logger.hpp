#pragma once
#include "types.hpp"
#include "protocol.hpp"
#include "connectivity.hpp"
#include "ring_buffer.hpp"
#include <cassert>
#include <cstdlib>
#include <windows.h>
#include <atomic>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <ostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

inline std::string make_timestamped_filename(const std::string& dir) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now_t);
#else
    localtime_r(&now_t, &tm); 
#endif

    std::ostringstream oss;
    oss << dir << "/"
        << std::put_time(&tm, "%Y%m%d_%H%M%S")
        << ".bin";
    return oss.str();
}

class BinaryEventLogger {
    public:
        explicit BinaryEventLogger(const std::string& file)
            : running_(true) {

            file_ = ::CreateFileA(
                file.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (file_ == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to open binary log file");
            }

            ::SetFilePointer(file_, 0, nullptr, FILE_END);

            writer_ = std::thread(&BinaryEventLogger::writer_loop, this);
        }

        ~BinaryEventLogger() {
            running_.store(false, std::memory_order_release);

            if (writer_.joinable()) {
                writer_.join();
            }

            if (file_ != INVALID_HANDLE_VALUE) {
                ::FlushFileBuffers(file_);
                ::CloseHandle(file_);
            }
        }

        void log_message(MessageType type, const void* payload) noexcept {
            const uint16_t payload_size = payload_size_for_type(type);

            MessageHeader header{ type, payload_size };

            uint8_t buffer[sizeof(MessageHeader) + MAX_PAYLOAD_SIZE];
            std::memcpy(buffer, &header, sizeof(header));
            std::memcpy(buffer + sizeof(header), payload, payload_size);

            const size_t total = sizeof(header) + payload_size;

            if (!queue_.try_push(buffer, total)) {
                // drop / count / backpressure
            }
        }


    private:
        void writer_loop() {
            alignas(64) uint8_t staging[65536];
            size_t offset = 0;

            while (running_.load(std::memory_order_acquire)) {
                auto [ptr, len] = queue_.peek();

                if (len > 0) {
                    const size_t to_copy = std::min(len, sizeof(staging) - offset);

                    std::memcpy(staging + offset, ptr, to_copy);
                    offset += to_copy;

                    queue_.advance_read_index(to_copy);

                    if (offset == sizeof(staging)) {
                        flush_to_disk(staging, offset);
                        offset = 0;
                    }
                } else {
                    if (offset > 0) {
                        flush_to_disk(staging, offset);
                        offset = 0;
                    }
                    _mm_pause();
                }
            }

            if (offset > 0) {
                flush_to_disk(staging, offset);
            }
        }


        void flush_to_disk(const void* buffer, size_t size) {
            DWORD written = 0;
            ::WriteFile(file_, buffer, static_cast<DWORD>(size), &written, nullptr);
        }

        RingBuffer<1 << 22> queue_; 
        std::atomic<bool> running_;
        std::thread writer_;
        HANDLE file_{INVALID_HANDLE_VALUE};
};