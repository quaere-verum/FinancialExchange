#pragma once
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>

#include "connectivity.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "protocol.hpp"

namespace error = boost::asio::error;
namespace ip = boost::asio::ip;
using boost::asio::ip::tcp;

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")

Connection::Connection(
    boost::asio::io_context& context,
    tcp::socket&& socket,
    Id_t id,
    boost::asio::strand<boost::asio::any_io_executor>* on_message_strand
)
    : context_(context)
    , socket_(std::move(socket))
    , id_(id)
    , read_buffer_(std::make_unique<uint8_t[]>(READ_SIZE))
    , io_strand_(socket_.get_executor())
    , on_message_strand_(on_message_strand)
    {}

Connection::~Connection() {
    close();
}

void Connection::close() { boost::system::error_code ec; socket_.close(ec); }

bool Connection::send_raw(const uint8_t* data, size_t len) noexcept {
    const bool pushed = send_queue_.try_push(data, len);
    if (!pushed) {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " send queue full, dropping " << len << " bytes.";
        return false; // drop / count / disconnect policy
    }
    RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " queued " << len << " bytes for send.";

    bool expected = false;
    if (write_in_progress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        boost::asio::post(
            io_strand_,
            [this]() { start_write(); }
        );
    }
    return true;
}

void Connection::start_write() {
    auto [ptr, len] = send_queue_.peek();
    if (len == 0) {
        write_in_progress_.store(false, std::memory_order_release);
        return;
    }
    RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " starting async_write_some of " << len << " bytes.";
    boost::asio::const_buffer buffer(ptr, len);

    socket_.async_write_some(
        buffer,
        [this](const boost::system::error_code& ec, size_t n) {
            handle_write(ec, n);
        }
    );
}

void Connection::handle_write(const boost::system::error_code& ec, size_t n) {
    if (ec) {
        if (ec != boost::asio::error::would_block &&
            ec != boost::asio::error::try_again &&
            ec != boost::asio::error::interrupted) {
            RLOG(LG_CON, LogLevel::LL_ERROR) << id_ << " write error: " << ec.message();
            on_disconnect();
            return;
        }
        RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " transient write error: " << ec.message();
        // transient error: retry
    } else {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " wrote " << n << " bytes.";
        send_queue_.advance_read_index(n);
    }

    auto [ptr, len] = send_queue_.peek();
    if (len > 0) {
        start_write();
    } else {
        write_in_progress_.store(false, std::memory_order_release);

        if (send_queue_.peek().second > 0) {
            bool expected = false;
            if (write_in_progress_.compare_exchange_strong(expected, true)) {
                boost::asio::dispatch(io_strand_, [this] {
                    start_write();
                });
            }
        }
    }
}


void Connection::async_read() {
    boost::asio::dispatch(io_strand_, [this]()
    {
        socket_.async_read_some(
            boost::asio::buffer(read_buffer_.get(), READ_SIZE),
            [this](const boost::system::error_code& ec, size_t n) {
                handle_read(ec, n);
            }
        );
    });
}

void Connection::handle_read(const boost::system::error_code& ec, size_t n) {
    if (ec) {
        RLOG(LG_CON, LogLevel::LL_ERROR) << id_ << " read error: " << ec.message();
        on_disconnect();
        return;
    }
    RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " received " << n << " bytes.";
    parse_messages(read_buffer_.get(), n);
    async_read();
}

void Connection::parse_messages(const uint8_t* buf, size_t n) {
    size_t offset = 0;

    while (offset + sizeof(MessageHeader) <= n) {
        Message_t type = static_cast<Message_t>(buf[offset]);
        uint16_t payload_size;
        std::memcpy(&payload_size, buf + offset + 1, sizeof(payload_size));

        if (offset + sizeof(MessageHeader) + payload_size > n) {
            break; // incomplete message, wait for more bytes
        }

        RLOG(LG_CON, LogLevel::LL_DEBUG)
            << id_ << " parsed message type="
            << static_cast<int>(type)
            << " payload_size=" << payload_size;
        const uint8_t* payload_ptr = buf + offset + sizeof(MessageHeader);

        std::vector<uint8_t> payload(payload_ptr, payload_ptr + payload_size);
        boost::asio::post(*on_message_strand_, 
            [this, type, payload = std::move(payload)]() mutable {
                on_message_received(type, payload.data());
            }
        );

        offset += sizeof(MessageHeader) + payload_size;
    }
}

void Connection::send_message(Message_t type, const void* payload) noexcept {
    uint16_t payload_size = payload_size_for_type(static_cast<MessageType>(type));
    uint8_t buffer[sizeof(MessageHeader) + MAX_PAYLOAD_SIZE];
    MessageHeader header{ static_cast<MessageType>(type), payload_size };
    std::memcpy(buffer, &header, sizeof(header));
    std::memcpy(buffer + sizeof(header), payload, payload_size);
     RLOG(LG_CON, LogLevel::LL_DEBUG)
        << id_ << " sending message type="
        << static_cast<int>(type)
        << " payload_size=" << payload_size;

    if (!send_raw(buffer, sizeof(header) + payload_size)) {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << id_ << " failed to enqueue message";
    }
}
