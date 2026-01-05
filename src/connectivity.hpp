#pragma once
#include "types.hpp"
#include <functional>
#include <vector>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/strand.hpp>

using boost::asio::ip::tcp;

constexpr size_t MESSAGE_HEADER_SIZE = 3;


class Connection : public IConnection {
    public:
        Connection(boost::asio::io_context& context, tcp::socket&& socket, Id_t id, boost::asio::strand<boost::asio::any_io_executor>* on_message_strand);
        ~Connection() override;
        void async_read() override;
        void send_message(Message_t message_type, const void* payload, SendMode mode) override;
        void close();
        Id_t id() {return id_;};

    private:
        void send();
        void send(SendMode mode);

        void read_some_handler(const boost::system::error_code& error, size_t size);
        void write_some_handler(const boost::system::error_code& error, size_t size);

        boost::asio::io_context& context_;
        boost::asio::streambuf in_buffer_;
        boost::asio::streambuf out_buffer_;
        bool is_sending_ = false;
        bool is_send_posted_ = false;
        tcp::socket socket_;
        Id_t id_;
        boost::asio::strand<boost::asio::any_io_executor> io_strand_;
        boost::asio::strand<boost::asio::any_io_executor>* on_message_strand_;
};
