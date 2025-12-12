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
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/system/error_code.hpp>

#include "connectivity.hpp"
#include "error.hpp"
#include "logging.hpp"
#include "protocol.hpp"

namespace error = boost::asio::error;
namespace interprocess = boost::interprocess;
namespace ip = boost::asio::ip;
using boost::asio::ip::tcp;

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")


// Theoretical maximum size of an (IPv4) UDP packet (actual maximum is lower).
constexpr size_t READ_SIZE = 65535;

Connection::Connection(boost::asio::io_context& context, tcp::socket&& socket, Id_t id)
    : context_(context),
      in_buffer_(),
      out_buffer_(),
      socket_(std::move(socket)),
      id_(id) {
        set_name('\'' + std::to_string(socket_.local_endpoint().port()) + '\'');
    }

Connection::~Connection() {
    RLOG(LG_CON, LogLevel::LL_INFO) << std::quoted(name_, '\'') << " closing";
    if (socket_.is_open()) {
        socket_.close();
    }
}

void Connection::async_read() {
    auto buf = in_buffer_.prepare(READ_SIZE);
    socket_.async_read_some(buf, [this](auto& error, auto size) { read_some_handler(error, size); });
}

void Connection::close() {
    boost::system::error_code ec;
    socket_.close(ec);
}

void Connection::read_some_handler(const boost::system::error_code& error, size_t size) {
    if (error) {
        if (error == error::eof) {
            RLOG(LG_CON, LogLevel::LL_INFO) << std::quoted(name_, '\'') << " remote disconnect";
        } else if (error == error::interrupted || error == error::try_again || error == error::would_block) {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << std::quoted(name_, '\'') << " read interrupted: " << error.message();
            async_read();
            return;
        } else {
            RLOG(LG_CON, LogLevel::LL_ERROR) << std::quoted(name_, '\'') << " read error: " << error.message();
        }
        on_disconnect();
        return;
    }

    RLOG(LG_CON, LogLevel::LL_DEBUG) << std::quoted(name_, '\'') << " received " << size << " bytes";
    in_buffer_.commit(size);

    auto bufs = in_buffer_.data();
    auto it = boost::asio::buffers_begin(bufs);
    auto end = boost::asio::buffers_end(bufs);

    const uint8_t* begin = reinterpret_cast<const uint8_t*>(&(*it));
    const uint8_t* upto = begin;

    size_t available = in_buffer_.size();
    
    while (available >= MESSAGE_HEADER_SIZE) {
        uint8_t message_type = upto[0];
        uint16_t payload_size = (static_cast<uint16_t>(upto[1]) << 8) | static_cast<uint16_t>(upto[2]); // read big-endian uint16_t length stored at offset 1..2
        size_t expected_payload_size = payload_size_for_type(static_cast<MessageType>(message_type));

        if ((static_cast<size_t>(payload_size) != expected_payload_size) && (expected_payload_size > 0)) {
            RLOG(LG_CON, LogLevel::LL_ERROR)
                << std::quoted(name_, '\'') << " invalid payload size=" << payload_size;
            on_disconnect();
            return;
        }

        if (available < payload_size + MESSAGE_HEADER_SIZE) {
            break;
        }
        const uint8_t* payload_ptr = upto + MESSAGE_HEADER_SIZE;

        RLOG(LG_CON, LogLevel::LL_DEBUG)
            << std::quoted(name_, '\'')
            << " received message with type=" << static_cast<int>(message_type)
            << " and size=" << payload_size;

        on_message_received(message_type, payload_ptr);

        upto += payload_size + MESSAGE_HEADER_SIZE;
        available -= payload_size + MESSAGE_HEADER_SIZE;
    }

    in_buffer_.consume(upto - begin);
    async_read();
}

void Connection::send() {
    is_sending_ = true;
    socket_.async_write_some(out_buffer_.data(), [this](auto& err, auto sz) { write_some_handler(err, sz); });
}

void Connection::send(SendMode mode) {
    if (mode == SendMode::ASAP) {
        send();
    } else if (!is_send_posted_) {
        boost::asio::post(context_, [this] {
            is_send_posted_ = false;
            if (!is_sending_) {
                send();
            }
        });
        is_send_posted_ = true;
    }
}

void Connection::send_message(Message_t message_type, const void* payload, SendMode mode) {
    const size_t payload_size = payload_size_for_type(static_cast<MessageType>(message_type));
    auto buf = out_buffer_.prepare(payload_size + MESSAGE_HEADER_SIZE);
    auto* data = static_cast<uint8_t*>(buf.data());
    data[0] = static_cast<uint8_t>(message_type);
    data[1] = static_cast<uint8_t>((payload_size >> 8) & 0xFF); // big-endian hi byte
    data[2] = static_cast<uint8_t>( payload_size & 0xFF); // big-endian lo byte
    std::memcpy(data + MESSAGE_HEADER_SIZE, payload, payload_size);
    out_buffer_.commit(payload_size + MESSAGE_HEADER_SIZE);
    if (!is_sending_) {
        send(mode);
    }
}

void Connection::write_some_handler(const boost::system::error_code& error, size_t size) {
    if (error) {
        if (error != error::interrupted && error != error::would_block && error != error::try_again) {
            RLOG(LG_CON, LogLevel::LL_ERROR) << std::quoted(name_, '\'') << " send failed: " << error.message();
            throw TGError("send failed: " + error.message());
        }
        RLOG(LG_CON, LogLevel::LL_DEBUG) << std::quoted(name_, '\'') << " send interrupted: " << error.message();
    } else {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << std::quoted(name_, '\'') << " sent " << size << " bytes";
        out_buffer_.consume(size);
    }

    if (out_buffer_.size() > 0) {
        socket_.async_write_some(out_buffer_.data(), [this](auto& err, auto sz) {write_some_handler(err, sz); });
    } else {
        is_sending_ = false;
    }
}
