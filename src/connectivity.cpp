#include "connectivity.hpp"
#include <boost/asio/write.hpp>
#include <algorithm>
#include "logging.hpp"

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")

Connection::Connection(
    boost::asio::io_context& context,
    tcp::socket&& socket,
    Id_t id,
    InboundQueue& inbound_to_engine,
    OutboundQueue& outbound_from_engine
)
  : context_(context)
  , socket_(std::move(socket))
  , id_(id)
  , io_strand_(socket_.get_executor())
  , inbound_to_engine_(inbound_to_engine)
  , outbound_from_engine_(outbound_from_engine) {
        in_accum_.resize(READ_SIZE * 2);
        in_used_ = 0;
    }

Connection::~Connection() {
    close();
}

void Connection::close() {
    boost::system::error_code ec;
    socket_.close(ec);
    if (ec) {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_ << " socket.close error: " << ec.message() << '\n';
    } else {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_ << " socket closed\n";
    }
}

void Connection::notify_disconnect_once_(const boost::system::error_code& ec) {
    bool expected = false;
    if (!disconnect_notified_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_ << " disconnect notified: " << ec.message() << '\n';

    close();

    if (disconnected) {
        disconnected(this);
    }
}

void Connection::notify_inbound_ready_() noexcept {
    if (!inbound_ready) return;

    bool expected = false;
    if (!inbound_ready_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return; // already scheduled
    }

    boost::asio::post(io_strand_, [this] {
        inbound_ready_pending_.store(false, std::memory_order_release);
        if (inbound_ready && !disconnect_notified_.load(std::memory_order_acquire)) {
            inbound_ready();
        }
    });
}

void Connection::async_read() {
    boost::asio::dispatch(io_strand_, [this] { start_read_(); });
}

void Connection::start_read_() {
  socket_.async_read_some(
      boost::asio::buffer(read_tmp_.data(), read_tmp_.size()),
      boost::asio::bind_executor(
          io_strand_,
          [this](const boost::system::error_code& ec, size_t n) {
            handle_read_(ec, n);
          }
      )
  );
}

void Connection::handle_read_(const boost::system::error_code& ec, size_t n) {
    if (ec) {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_ << " read error/disconnect: "
               << ec.message() << " (bytes_read=" << n << ")\n";
        notify_disconnect_once_(ec);
        return;
    }

    if (in_used_ + n > in_accum_.size()) {
        size_t new_sz = in_accum_.size();
        while (new_sz < in_used_ + n) {
            new_sz *= 2;
        }
        in_accum_.resize(new_sz);
    }

    std::memcpy(in_accum_.data() + in_used_, read_tmp_.data(), n);
    in_used_ += n;

    parse_accumulator_();
    if (!disconnect_notified_.load(std::memory_order_acquire)) {
        start_read_();
    }
}

void Connection::parse_accumulator_() {
    size_t offset = 0;

    while (true) {
        if (in_used_ - offset < WIRE_HEADER_SIZE) {
            break;
        }

        const uint8_t type_u8 = in_accum_[offset + 0];
        const uint16_t payload_size = read_u16_be(in_accum_.data() + offset + 1);

        const size_t frame_sz = WIRE_HEADER_SIZE + payload_size;
        if (in_used_ - offset < frame_sz) {
            break; // partial frame, wait for more bytes
        }

        if (payload_size > MAX_PAYLOAD_SIZE) {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                   << " protocol violation: payload_size=" << payload_size
                   << " > MAX_PAYLOAD_SIZE=" << MAX_PAYLOAD_SIZE
                   << " (type_u8=" << static_cast<unsigned>(type_u8)
                   << "); closing\n";
            notify_disconnect_once_(boost::asio::error::fault);
            return;
        }

        const Message_t message_type = static_cast<Message_t>(static_cast<MessageType>(type_u8));
        const uint8_t* payload_ptr = in_accum_.data() + offset + WIRE_HEADER_SIZE;

        if (payload_size <= MAX_PAYLOAD_SIZE_BUFFER) {
            InboundMessage msg{};
            msg.connection_id = id_;
            msg.message_type = message_type;
            msg.payload_size = payload_size;
            if (payload_size) {
                std::memcpy(msg.payload.data(), payload_ptr, payload_size);
            }

            if (!inbound_to_engine_.try_push(msg)) {
                // Backpressure policy: disconnect on sustained overload.
                RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                       << " inbound queue backpressure: try_push failed "
                       << "(type_u8=" << static_cast<unsigned>(type_u8)
                       << " payload_size=" << payload_size
                       << "); closing\n";
                notify_disconnect_once_(boost::asio::error::no_buffer_space);
                return;
            }
            notify_inbound_ready_();
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                   << " inbound frame queued: type_u8=" << static_cast<unsigned>(type_u8)
                   << " payload_size=" << payload_size
                   << " frame_sz=" << frame_sz
                   << '\n';
            offset += frame_sz;
        } else {
            if (large_message_received) {
                RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                   << " large inbound frame: type_u8=" << static_cast<unsigned>(type_u8)
                   << " payload_size=" << payload_size
                   << " (unbuffered callback path)\n";
                auto buf = std::make_shared<std::vector<uint8_t>>(payload_size);
                if (payload_size) std::memcpy(buf->data(), payload_ptr, payload_size);
                large_message_received(id_, message_type, std::move(buf));
            }
            offset += frame_sz;
        }

    }

    if (offset > 0) {
        const size_t remaining = in_used_ - offset;
        if (remaining) {
            std::memmove(in_accum_.data(), in_accum_.data() + offset, remaining);
        }
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
               << " parse consumed " << offset
               << " bytes; remaining=" << remaining
               << '\n';
        in_used_ = remaining;
    }
}

void Connection::send_message(Message_t type, const void* payload) noexcept {
    const uint16_t payload_size =
        payload_size_for_type(static_cast<MessageType>(type));

    // Enforce buffered size (order book snapshot excluded by design)
    if (payload_size > MAX_PAYLOAD_SIZE_BUFFER) {
        return;
    }

    OutboundMessage msg{};
    msg.connection_id = id_;
    msg.message_type = type;
    msg.payload_size = payload_size;
    if (payload_size) {
        std::memcpy(msg.payload.data(), payload, payload_size);
    }

    if (!outbound_from_engine_.try_push(msg)) {
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
               << " outbound queue backpressure: try_push failed "
               << "(type=" << static_cast<unsigned>(type)
               << " payload_size=" << payload_size << ")\n";
        return;
    }

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
           << " outbound queued: type=" << static_cast<unsigned>(type)
           << " payload_size=" << payload_size
           << '\n';

    schedule_drain_writes_();
}

void Connection::send_message_unbuffered(Message_t type, const void* payload, uint16_t payload_size) noexcept {
    if (payload_size == 0 || payload == nullptr) {
        return;
    }

    constexpr size_t header_size = 1 + 2;
    const size_t frame_size = header_size + static_cast<size_t>(payload_size);

    // Allocate heap buffer so it stays alive until async_write completes.
    auto buffer = std::make_shared<std::vector<uint8_t>>(frame_size);
    (*buffer)[0] = static_cast<uint8_t>(static_cast<MessageType>(type));

    // Serialize payload size as little-endian u16
    (*buffer)[1] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
    (*buffer)[2] = static_cast<uint8_t>(payload_size & 0xFF);


    std::memcpy(buffer->data() + header_size, payload, payload_size);

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
           << " send_message_unbuffered scheduled: type=" << static_cast<unsigned>(type)
           << " payload_size=" << payload_size
           << " frame_size=" << frame_size
           << '\n';

    boost::asio::post(io_strand_, [this, buffer]() {
    if (!socket_.is_open()) return;

    boost::asio::async_write(
        socket_,
        boost::asio::buffer(*buffer),
        boost::asio::bind_executor(
            io_strand_,
            [this, buffer](const boost::system::error_code& ec, size_t /*n*/) {
                if (ec) {
                    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                               << " unbuffered write error/disconnect: "
                               << ec.message() << "\n";
                    notify_disconnect_once_(ec);
                }
            }));
    });
    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                           << " unbuffered write complete: bytes_written=" << frame_size
                           << " (type=" << type
                           << " payload_size=" << payload_size << ")\n";
}


void Connection::schedule_drain_writes_() noexcept {
    bool expected = false;
    if (write_wakeup_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        boost::asio::post(io_strand_, [this] { drain_writes_(); });
    }
}

void Connection::drain_writes_() {
    write_wakeup_pending_.store(false, std::memory_order_release);

    if (write_in_progress_) {
        return;
    }

    out_batch_len_ = 0;
    out_batch_sent_ = 0;

    while (true) {
        const OutboundMessage* m = outbound_from_engine_.peek();
        if (!m) break;

        const uint16_t psz = m->payload_size;
        const size_t frame_sz = WIRE_HEADER_SIZE + psz;

        if (out_batch_len_ + frame_sz > out_batch_.size()) {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
                   << " drain_writes_: batch full at len=" << out_batch_len_
                   << " next_frame_sz=" << frame_sz
                   << " batch_capacity=" << out_batch_.size()
                   << '\n';
            break;
        }

        out_batch_[out_batch_len_ + 0] = static_cast<uint8_t>(static_cast<MessageType>(m->message_type));
        write_u16_be(out_batch_.data() + out_batch_len_ + 1, psz);
        if (psz) {
            std::memcpy(out_batch_.data() + out_batch_len_ + WIRE_HEADER_SIZE, m->payload.data(), psz);
        }

        out_batch_len_ += frame_sz;
        outbound_from_engine_.consume_one();
        }

        if (out_batch_len_ == 0) {
        return;
    }

    start_write_();
}

void Connection::start_write_() {
    write_in_progress_ = true;

    const size_t remaining = out_batch_len_ - out_batch_sent_;
    socket_.async_write_some(
        boost::asio::buffer(out_batch_.data() + out_batch_sent_, remaining),
        boost::asio::bind_executor(
            io_strand_,
            [this](const boost::system::error_code& ec, size_t n) {
            handle_write_(ec, n);
            }
        )
    );
}

void Connection::handle_write_(const boost::system::error_code& ec, size_t n) {
    if (ec) {
        write_in_progress_ = false;
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_
               << " write error/disconnect: " << ec.message()
               << " (bytes_written=" << n
               << " batch_sent=" << out_batch_sent_
               << " batch_len=" << out_batch_len_
               << ")\n";
        notify_disconnect_once_(ec);
        return;
    }

    out_batch_sent_ += n;

    if (out_batch_sent_ < out_batch_len_) {
        start_write_();
        return;
    }

    write_in_progress_ = false;

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "conn=" << id_ << " write batch complete: total=" << out_batch_len_ << '\n';; 

    if (outbound_from_engine_.peek() != nullptr) {
        drain_writes_();
        return;
    }

    bool expected = true;
    if (write_wakeup_pending_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        drain_writes_();
    }
}
