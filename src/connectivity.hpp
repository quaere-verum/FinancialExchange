#pragma once
#include "types.hpp"
#include "protocol.hpp"
#include "ring_buffer.hpp"
#include <functional>
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/strand.hpp>

using boost::asio::ip::tcp;
constexpr size_t READ_SIZE = 65535;

inline void serialize_message(
    uint8_t* dst,
    MessageType type,
    const void* payload,
    uint16_t payload_size
) {
    dst[0] = static_cast<uint8_t>(type);

    std::memcpy(dst + 1, &payload_size, sizeof(payload_size));

    std::memcpy(dst + sizeof(MessageHeader), payload, payload_size);
}


class Connection {
    public:
        Connection(
            boost::asio::io_context& context,
            tcp::socket&& socket,
            Id_t id,
            boost::asio::strand<boost::asio::any_io_executor>* on_message_strand
        );

        ~Connection();

        void async_read();
        void close();

        bool send_raw(const uint8_t* data, size_t len) noexcept;
        void send_message(Message_t type, const void* payload) noexcept;
        Id_t id() const noexcept { return id_; }

    public:
        std::function<void(Connection*)> disconnected;
        std::function<void(Connection*, Message_t, const uint8_t*)> message_received;

    private:
        void start_write();
        void handle_write(const boost::system::error_code&, size_t);
        void handle_read(const boost::system::error_code&, size_t);
        void parse_messages(const uint8_t* buf, size_t n);
        void on_disconnect() {if (disconnected) {disconnected(this);}}
        void on_message_received(Message_t message_type, const uint8_t* data) {
            if (message_received) {message_received(this, message_type, data);}
        }

    private:
        boost::asio::io_context& context_;
        tcp::socket socket_;
        Id_t id_;

        alignas(64) std::unique_ptr<uint8_t[]> read_buffer_;

        RingBuffer<1 << 20> send_queue_;
        std::atomic<bool> write_in_progress_{false};

        boost::asio::strand<boost::asio::any_io_executor> io_strand_;
        boost::asio::strand<boost::asio::any_io_executor>* on_message_strand_;
};
