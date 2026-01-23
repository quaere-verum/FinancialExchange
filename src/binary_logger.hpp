#pragma once

#include "types.hpp"
#include "protocol.hpp"
#include "spsc_queue.hpp"
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <algorithm>

// ------------------------------------------------------------
// Filenames
// ------------------------------------------------------------
inline std::string make_timestamp_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now_t);
#else
    localtime_r(&now_t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

inline std::string message_type_to_string(MessageType t) {
    switch (t) {
        case MessageType::PRICE_LEVEL_UPDATE:    return "price_level_update";
        case MessageType::TRADE_EVENT:           return "trade";
        case MessageType::ORDER_INSERTED_EVENT:  return "insert_order";
        case MessageType::ORDER_CANCELLED_EVENT: return "cancel_order";
        case MessageType::ORDER_AMENDED_EVENT:   return "amend_order";
        default: {
            std::ostringstream oss;
            oss << "type_" << static_cast<uint32_t>(t);
            return oss.str();
        }
    }
}

inline std::string make_typed_filename(
    const std::string& dir,
    const std::string& base_ts,
    MessageType type
) {
    std::ostringstream oss;
    oss << dir << "/"
        << base_ts << "_"
        << message_type_to_string(type)
        << ".bin";
    return oss.str();
}

constexpr size_t MAX_LOGGED_SIZE = []() {
    size_t sizes[] = {
        sizeof(PayloadTradeEvent),
        sizeof(PayloadOrderInsertedEvent),
        sizeof(PayloadOrderCancelledEvent),
        sizeof(PayloadOrderAmendedEvent),
        sizeof(PayloadPriceLevelUpdate)
    };
    size_t m = 0;
    for (size_t s : sizes) if (s > m) m = s;
    return m;
}();

// ------------------------------------------------------------
// Logger
// ------------------------------------------------------------
//
// Design:
// - One file per message type, payload-only (no per-message header).
// - One SPSCQueue per message type, with a fixed-size item buffer sized
//   to the maximum payload size among the known logged message types.
//   Only the first payload_size_for_type(type) bytes are written.
// - Single writer thread drains all queues and flushes per-type staging buffers.
//
class BinaryEventLogger {
    public:
        explicit BinaryEventLogger(const std::string& dir)
            : dir_(dir),
            base_ts_(make_timestamp_string()),
            running_(true) {

            open_sink_(MessageType::PRICE_LEVEL_UPDATE, sink_plu_);
            open_sink_(MessageType::TRADE_EVENT,        sink_trade_);
            open_sink_(MessageType::ORDER_INSERTED_EVENT,  sink_insert_);
            open_sink_(MessageType::ORDER_CANCELLED_EVENT, sink_cancel_);
            open_sink_(MessageType::ORDER_AMENDED_EVENT,   sink_amend_);

            writer_ = std::thread(&BinaryEventLogger::writer_loop, this);
        }

        ~BinaryEventLogger() {
            running_.store(false, std::memory_order_release);
            if (writer_.joinable()) writer_.join();

            // Final flush & close
            flush_sink_(sink_plu_);
            flush_sink_(sink_trade_);
            flush_sink_(sink_insert_);
            flush_sink_(sink_cancel_);
            flush_sink_(sink_amend_);

            close_sink_(sink_plu_);
            close_sink_(sink_trade_);
            close_sink_(sink_insert_);
            close_sink_(sink_cancel_);
            close_sink_(sink_amend_);
        }

        BinaryEventLogger(const BinaryEventLogger&) = delete;
        BinaryEventLogger& operator=(const BinaryEventLogger&) = delete;

        // Producer-side entry point. Copies payload bytes into the appropriate queue.
        // Drops on overflow.
        void log_message(MessageType type, const void* payload) noexcept {
            switch (type) {
                case MessageType::PRICE_LEVEL_UPDATE: {
                    PayloadItem item{};
                    std::memcpy(item.bytes, payload, sink_plu_.payload_size);
                    (void)q_plu_.try_push(item);
                    break;
                }
                case MessageType::TRADE_EVENT: {
                    PayloadItem item{};
                    std::memcpy(item.bytes, payload, sink_trade_.payload_size);
                    (void)q_trade_.try_push(item);
                    break;
                }
                case MessageType::ORDER_INSERTED_EVENT: {
                    PayloadItem item{};
                    std::memcpy(item.bytes, payload, sink_insert_.payload_size);
                    (void)q_insert_.try_push(item);
                    break;
                }
                case MessageType::ORDER_CANCELLED_EVENT: {
                    PayloadItem item{};
                    std::memcpy(item.bytes, payload, sink_cancel_.payload_size);
                    (void)q_cancel_.try_push(item);
                    break;
                }
                case MessageType::ORDER_AMENDED_EVENT: {
                    PayloadItem item{};
                    std::memcpy(item.bytes, payload, sink_amend_.payload_size);
                    (void)q_amend_.try_push(item);
                    break;
                }
                default:
                    // Unknown/unlogged type: ignore
                    break;
            }
        }

        size_t backlog_approx() const noexcept {
            return q_plu_.size_approx()
                + q_trade_.size_approx()
                + q_insert_.size_approx()
                + q_cancel_.size_approx()
                + q_amend_.size_approx();
        }

    private:

        struct PayloadItem {
            uint8_t bytes[MAX_LOGGED_SIZE]{};
        };
        static_assert(std::is_trivially_copyable_v<PayloadItem>);

        struct FileSink {
            HANDLE file{INVALID_HANDLE_VALUE};
            static constexpr size_t STAGING_BYTES = 64 * 1024;
            uint8_t staging[STAGING_BYTES]{};
            size_t offset{0};
            uint16_t payload_size{0};
            bool opened{false};
        };


        // Tune per-type queue depths. Price-level updates often dominate.
        static constexpr size_t Q_PLU_CAP   = 1u << 15; // 65k
        static constexpr size_t Q_TRADE_CAP = 1u << 15; // 16k
        static constexpr size_t Q_MISC_CAP  = 1u << 14; // 16k

        SPSCQueue<PayloadItem, Q_PLU_CAP>   q_plu_{};
        SPSCQueue<PayloadItem, Q_TRADE_CAP> q_trade_{};
        SPSCQueue<PayloadItem, Q_MISC_CAP>  q_insert_{};
        SPSCQueue<PayloadItem, Q_MISC_CAP>  q_cancel_{};
        SPSCQueue<PayloadItem, Q_MISC_CAP>  q_amend_{};

        void writer_loop() {
            constexpr int BATCH = 256;

            PayloadItem item{};

            while (running_.load(std::memory_order_acquire) ||
                backlog_approx() > 0) {

                bool did_work = false;

                did_work |= drain_queue_(q_plu_,   sink_plu_,   item, BATCH);
                did_work |= drain_queue_(q_trade_, sink_trade_, item, BATCH);
                did_work |= drain_queue_(q_insert_, sink_insert_, item, BATCH);
                did_work |= drain_queue_(q_cancel_, sink_cancel_, item, BATCH);
                did_work |= drain_queue_(q_amend_,  sink_amend_,  item, BATCH);

                if (!did_work) {
                    // If nothing was drained, flush any partial buffers opportunistically
                    // (keeps latency bounded without busy writing too often).
                    flush_sink_if_nonempty_(sink_plu_);
                    flush_sink_if_nonempty_(sink_trade_);
                    flush_sink_if_nonempty_(sink_insert_);
                    flush_sink_if_nonempty_(sink_cancel_);
                    flush_sink_if_nonempty_(sink_amend_);
                    _mm_pause();
                }
            }

            flush_sink_(sink_plu_);
            flush_sink_(sink_trade_);
            flush_sink_(sink_insert_);
            flush_sink_(sink_cancel_);
            flush_sink_(sink_amend_);
        }

        template <size_t CapPow2>
        bool drain_queue_(
            SPSCQueue<PayloadItem, CapPow2>& q,
            FileSink& sink,
            PayloadItem& tmp,
            int batch
        ) noexcept {
            bool did = false;
            const uint16_t psz = sink.payload_size;

            for (int i = 0; i < batch; ++i) {
                if (!q.try_pop(tmp)) break;

                did = true;
                if (psz > FileSink::STAGING_BYTES) {
                    flush_sink_(sink);
                    write_direct_(sink, tmp.bytes, psz);
                    continue;
                }

                if (sink.offset + psz > FileSink::STAGING_BYTES) {
                    flush_sink_(sink);
                }

                std::memcpy(sink.staging + sink.offset, tmp.bytes, psz);
                sink.offset += psz;

                // Heuristic: flush near full
                if (sink.offset >= FileSink::STAGING_BYTES - 4096) {
                    flush_sink_(sink);
                }
            }
            return did;
        }

        static void flush_sink_if_nonempty_(FileSink& sink) noexcept {
            if (sink.offset > 0 && sink.offset >= 4096) {
                flush_sink_(sink);
            }
        }

        void open_sink_(MessageType type, FileSink& sink) {
            const std::string filename = make_typed_filename(dir_, base_ts_, type);

            sink.file = ::CreateFileA(
                filename.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (sink.file == INVALID_HANDLE_VALUE) {
                throw std::runtime_error("Failed to open binary log file: " + filename);
            }

            sink.payload_size = payload_size_for_type(type);
            if (sink.payload_size > MAX_LOGGED_SIZE) {
                throw std::runtime_error("Payload size exceeds MAX_LOGGED_SIZE for type: " + filename);
            }

            sink.opened = true;
            sink.offset = 0;
        }

        static void flush_sink_(FileSink& sink) noexcept {
            if (!sink.opened || sink.file == INVALID_HANDLE_VALUE) return;
            if (sink.offset == 0) return;

            DWORD written = 0;
            (void)::WriteFile(
                sink.file,
                sink.staging,
                static_cast<DWORD>(sink.offset),
                &written,
                nullptr
            );
            sink.offset = 0;
        }

        static void write_direct_(FileSink& sink, const void* buffer, size_t size) noexcept {
            if (!sink.opened || sink.file == INVALID_HANDLE_VALUE) return;
            DWORD written = 0;
            (void)::WriteFile(
                sink.file,
                buffer,
                static_cast<DWORD>(size),
                &written,
                nullptr
            );
        }

        static void close_sink_(FileSink& sink) noexcept {
            if (!sink.opened || sink.file == INVALID_HANDLE_VALUE) return;
            ::FlushFileBuffers(sink.file);
            ::CloseHandle(sink.file);
            sink.file = INVALID_HANDLE_VALUE;
            sink.opened = false;
            sink.offset = 0;
        }

    private:
        std::string dir_;
        std::string base_ts_;

        std::atomic<bool> running_{false};
        std::thread writer_;

        FileSink sink_plu_{};
        FileSink sink_trade_{};
        FileSink sink_insert_{};
        FileSink sink_cancel_{};
        FileSink sink_amend_{};
};
