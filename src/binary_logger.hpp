#pragma once
#include "windows.h"
#include "types.hpp"
#include "protocol.hpp"
#include "connectivity.hpp"
#include "types.hpp"
#include <fcntl.h>
#include <cassert>
#include <cerrno>
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


// template<typename T, size_t Capacity>
// class SpscRingBuffer {
//     static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
//     static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
//     public:
//         SpscRingBuffer() noexcept
//         : head_(0)
//         , tail_(0) 
//         {}

//         bool push(const T& item) noexcept {
//             const size_t head = head_.load(std::memory_order_relaxed);
//             const size_t next = head + 1;

//             if (next - tail_.load(std::memory_order_acquire) > Capacity) {
//                 return false;
//             }

//             buffer_[head & mask_] = item;
//             head_.store(next, std::memory_order_release);
//             return true;
//         }
        
//         bool pop(T& out) noexcept {
//             const size_t tail = tail_.load(std::memory_order_relaxed);

//             if (tail == head_.load(std::memory_order_acquire)) {
//                 return false;
//             }

//             out = buffer_[tail & mask_];
//             tail_.store(tail + 1, std::memory_order_release);
//             return true;
//         }

//     private:
//         static constexpr size_t mask_ = Capacity - 1;

//         alignas(64) std::atomic<size_t> head_;
//         alignas(64) std::atomic<size_t> tail_;
//         alignas(64) T buffer_[Capacity];
// };


// using LogBuffer = std::vector<uint8_t>;

// class BinaryEventLogger {
//     public:
        // explicit BinaryEventLogger(const std::string& file)
        //     : running_(true) {

        //     file_ = ::CreateFileA(
        //         file.c_str(),
        //         GENERIC_WRITE,
        //         FILE_SHARE_READ,
        //         nullptr,
        //         OPEN_ALWAYS,
        //         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        //         nullptr
        //     );

        //     if (file_ == INVALID_HANDLE_VALUE) {
        //         throw std::runtime_error("Failed to open binary log file");
        //     }

        //     ::SetFilePointer(file_, 0, nullptr, FILE_END);

        //     writer_ = std::thread(&BinaryEventLogger::writer_loop, this);
        // }

        // ~BinaryEventLogger() {
        //     running_.store(false, std::memory_order_release);

        //     if (writer_.joinable()) {
        //         writer_.join();
        //     }

        //     if (file_ != INVALID_HANDLE_VALUE) {
        //         ::FlushFileBuffers(file_);
        //         ::CloseHandle(file_);
        //     }
        // }

//         inline void log_message(
//             MessageType message_type,
//             const void* payload
//         ) noexcept {
//             uint16_t payload_size = payload_size_for_type(message_type);
//             auto* buf = new LogBuffer;
//             buf->resize(MESSAGE_HEADER_SIZE + payload_size);

//             serialize_message(
//                 buf->data(),
//                 message_type,
//                 static_cast<const uint8_t*>(payload),
//                 payload_size
//             );

//             queue_.push(buf);
//         }

//     private:
//         void writer_loop() {
//             constexpr size_t BATCH_SIZE = 1024;

//             std::vector<LogBuffer*> batch;
//             batch.reserve(BATCH_SIZE);

//             LogBuffer* buf = nullptr;

//             while (running_.load(std::memory_order_acquire)) {
//                 while (queue_.pop(buf)) {
//                     batch.push_back(buf);

//                     if (batch.size() == BATCH_SIZE) {
//                         flush_batch(batch);
//                     }
//                 }

//                 if (!batch.empty()) {
//                     flush_batch(batch);
//                 }

//                 std::this_thread::yield();
//             }

//             while (queue_.pop(buf)) {
//                 batch.push_back(std::move(buf));
//             }

//             if (!batch.empty()) {
//                 flush_batch(batch);
//             }
//         }

//         void flush_batch(std::vector<LogBuffer*>& batch) {
//             for (const auto& msg : batch) {
//                 DWORD written = 0;

//                 ::WriteFile(
//                     file_,
//                     msg->data(),
//                     static_cast<DWORD>(msg->size()),
//                     &written,
//                     nullptr
//                 );
//                 delete msg;
//                 // unrecoverable I/O errors are intentionally ignored
//             }

//             batch.clear();
//         }

//     private:
        // SpscRingBuffer<LogBuffer*, 1 << 14> queue_;
        // std::atomic<bool> running_;
        // std::thread writer_;
        // HANDLE file_{INVALID_HANDLE_VALUE};
// };


#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>

#pragma pack(push, 1)
struct MessageHeader {
    MessageType type;
    uint16_t size;
};
#pragma pack(pop)

template<size_t Capacity>
class ZeroAllocRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    public:

        bool try_push(MessageType type, const void* payload, uint16_t payload_size) noexcept {
            const size_t total_size = sizeof(MessageHeader) + payload_size;
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t tail = tail_.load(std::memory_order_acquire);

            if (Capacity - (head - tail) < total_size) {
                return false; 
            }

            MessageHeader header{ type, payload_size };
            copy_to_buffer(head, &header, sizeof(MessageHeader));
            
            copy_to_buffer(head + sizeof(MessageHeader), payload, payload_size);

            head_.store(head + total_size, std::memory_order_release);
            return true;
        }

        size_t try_pop(uint8_t* dest_buffer, size_t dest_capacity, MessageType& out_type) noexcept {
            const size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);

            if (tail == head) return 0;

            MessageHeader header;
            copy_from_buffer(tail, &header, sizeof(MessageHeader));

            if (dest_capacity < header.size) return 0; // Out of space in dest

            copy_from_buffer(tail + sizeof(MessageHeader), dest_buffer, header.size);
            out_type = header.type;

            tail_.store(tail + sizeof(MessageHeader) + header.size, std::memory_order_release);
            return header.size;
        }

    private:
        void copy_to_buffer(size_t pos, const void* src, size_t len) noexcept {
            const size_t idx = pos & mask_;
            const size_t to_end = Capacity - idx;
            if (to_end < len) {
                std::memcpy(&buffer_[idx], src, to_end);
                std::memcpy(&buffer_[0], (const uint8_t*)src + to_end, len - to_end);
            } else {
                std::memcpy(&buffer_[idx], src, len);
            }
        }

        void copy_from_buffer(size_t pos, void* dest, size_t len) noexcept {
            const size_t idx = pos & mask_;
            const size_t to_end = Capacity - idx;
            if (to_end < len) {
                std::memcpy(dest, &buffer_[idx], to_end);
                std::memcpy((uint8_t*)dest + to_end, &buffer_[0], len - to_end);
            } else {
                std::memcpy(dest, &buffer_[idx], len);
            }
        }

        static constexpr size_t mask_ = Capacity - 1;
        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};
        uint8_t buffer_[Capacity]; 
};


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
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
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
            uint16_t size = payload_size_for_type(type);

            
            if (!queue_.try_push(type, payload, size)) {
                // TODO: Implement this
            }
        }

    private:
        void writer_loop() {
            uint8_t staging_buffer[65536]; // 64KB buffer
            size_t current_offset = 0;
            MessageType msg_type;

            while (running_.load(std::memory_order_acquire)) {
                // Peek at the message size from the ring buffer...
                // (Assuming a slightly modified try_pop that returns data directly)
                
                uint8_t msg_payload[MAX_PAYLOAD_SIZE];
                size_t bytes_read = queue_.try_pop(msg_payload, sizeof(msg_payload), msg_type);

                if (bytes_read > 0) {
                    // Serialize header + payload into our staging buffer
                    MessageHeader header{msg_type, static_cast<uint16_t>(bytes_read) };
                    
                    // Ensure we don't overflow staging_buffer
                    if (current_offset + sizeof(header) + bytes_read > sizeof(staging_buffer)) {
                        flush_to_disk(staging_buffer, current_offset);
                        current_offset = 0;
                    }

                    std::memcpy(staging_buffer + current_offset, &header, sizeof(header));
                    current_offset += sizeof(header);
                    std::memcpy(staging_buffer + current_offset, msg_payload, bytes_read);
                    current_offset += bytes_read;
                } else {
                    // If the queue is empty, flush whatever we have and rest
                    if (current_offset > 0) {
                        flush_to_disk(staging_buffer, current_offset);
                        current_offset = 0;
                    }
                    _mm_pause(); 
                }
            }
        }

        void flush_to_disk(const void* buffer, size_t size) {
            DWORD written = 0;
            ::WriteFile(file_, buffer, static_cast<DWORD>(size), &written, nullptr);
        }

        ZeroAllocRingBuffer<1 << 22> queue_; 
        std::atomic<bool> running_;
        std::thread writer_;
        HANDLE file_{INVALID_HANDLE_VALUE};
};