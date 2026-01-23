#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <type_traits>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include "types.hpp"
#include "protocol.hpp"
#include "spsc_queue.hpp" // your SPSCQueue<T, N>

using boost::asio::ip::tcp;

constexpr size_t READ_SIZE = 65535;

static_assert(MAX_PAYLOAD_SIZE_BUFFER <= 64, "MAX_PAYLOAD_SIZE_BUFFER unexpectedly large; revisit queue sizing.");

static_assert(std::is_enum_v<MessageType>, "MessageType must be an enum.");
static_assert(std::is_same_v<std::underlying_type_t<MessageType>, std::uint8_t>,
              "Wire format assumes MessageType underlying type is uint8_t.");

struct InboundMessage {
  Id_t connection_id;
  Message_t message_type;
  uint16_t payload_size;
  std::array<std::uint8_t, MAX_PAYLOAD_SIZE_BUFFER> payload;
};

struct OutboundMessage {
  Id_t connection_id;
  Message_t message_type;
  uint16_t payload_size;
  std::array<std::uint8_t, MAX_PAYLOAD_SIZE_BUFFER> payload;
};

static_assert(std::is_trivially_copyable_v<InboundMessage>);
static_assert(std::is_trivially_copyable_v<OutboundMessage>);

constexpr size_t INBOUND_Q_CAP  = 4096;
constexpr size_t OUTBOUND_Q_CAP = 4096;

using InboundQueue  = SPSCQueue<InboundMessage, INBOUND_Q_CAP>;
using OutboundQueue = SPSCQueue<OutboundMessage, OUTBOUND_Q_CAP>;

class Connection {
public:
    Connection(
        boost::asio::io_context& context,
        tcp::socket&& socket,
        Id_t id,
        InboundQueue& inbound_to_engine, // produced by IO thread, consumed by engine thread
        OutboundQueue& outbound_from_engine // produced by engine thread, consumed by IO thread
    );

    ~Connection();

    void async_read();

    void send_message(Message_t type, const void* payload) noexcept;
    void send_message_unbuffered(Message_t type, const void* payload, uint16_t payload_size) noexcept;

    void close();
    Id_t id() const noexcept { return id_; }

public:
    std::function<void(Connection*)> disconnected;
    // Rare-path hook for payloads larger than MAX_PAYLOAD_SIZE_BUFFER.
    std::function<void(Id_t, Message_t, std::shared_ptr<std::vector<uint8_t>>)> large_message_received;
    std::function<void()> inbound_ready;



private:
    // I/O strand only
    void start_read_();
    void handle_read_(const boost::system::error_code& ec, size_t n);
    void parse_accumulator_();

    void schedule_drain_writes_() noexcept; // may be called cross-thread
    void drain_writes_(); // I/O strand only
    void start_write_(); // I/O strand only
    void handle_write_(const boost::system::error_code& ec, size_t n);

    void notify_inbound_ready_() noexcept;
    void notify_disconnect_once_(const boost::system::error_code& ec);
    inline void on_disconnect_() {
        if (disconnected) disconnected(this);
    }

    static constexpr size_t WIRE_HEADER_SIZE = 1 + 2; // type (u8) + size (u16)

    static inline void write_u16_be(uint8_t* dst, uint16_t v) noexcept {
        dst[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
        dst[1] = static_cast<uint8_t>(v & 0xFF);
    }

    static inline uint16_t read_u16_be(const uint8_t* src) noexcept {
        return (static_cast<uint16_t>(src[0]) << 8) | static_cast<uint16_t>(src[1]);
    }


private:
    boost::asio::io_context& context_;
    tcp::socket socket_;
    Id_t id_;

    boost::asio::strand<boost::asio::any_io_executor> io_strand_;

    InboundQueue& inbound_to_engine_;
    OutboundQueue& outbound_from_engine_;

    std::array<uint8_t, READ_SIZE> read_tmp_{};

    std::vector<uint8_t> in_accum_;
    size_t in_used_ = 0;

    std::array<uint8_t, 64 * 1024> out_batch_{};
    size_t out_batch_len_ = 0;
    size_t out_batch_sent_ = 0;

    bool write_in_progress_ = false;

    std::atomic<bool> write_wakeup_pending_{false};
    std::atomic<bool> disconnect_notified_{false};
    std::atomic<bool> inbound_ready_pending_{false};
};
