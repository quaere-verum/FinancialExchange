#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>
#include <unordered_map>
#include <memory>
#include <functional>
#include <vector>
#include "binary_logger.hpp"

#include "connectivity.hpp"
#include "types.hpp"
#include "protocol.hpp"
#include "order_book.hpp"
#include "callbacks.hpp"
#include "logging.hpp"

class Exchange : public OrderBookCallbacks {
    public:
        using tcp = boost::asio::ip::tcp;
        Exchange(boost::asio::io_context& context, uint16_t port);
        void start();
        void stop();
        void print_book() {order_book_.print_book();};
        void broadcast(Message_t message_type, const uint8_t* payload);
        void send_to(Connection* client, Message_t message_type, const void* payload);
        void subscribe_market_feed(Connection* client);
        void unsubscribe_market_feed(Connection* client);
        boost::asio::strand<boost::asio::any_io_executor>& Exchange::strand() noexcept {return strand_;}

        void on_trade(
            const Order& maker_order,
            Id_t taker_client_id,
            Id_t taker_order_id,
            Price_t price,
            Volume_t taker_total_quantity,
            Volume_t taker_cumulative_quantity,
            Volume_t traded_quantity,
            Time_t timestamp
        ) override;
        void on_order_inserted(Id_t client_request_id, const Order& order, Time_t timestamp) override;
        void on_order_cancelled(Id_t client_request_id, const Order& order, Time_t timestamp) override;
        void on_order_amended(Id_t client_request_id, Volume_t quantity_old, const Order& order, Time_t timestamp) override;
        void on_level_update(Side side, PriceLevel const& level, Time_t timestamp) override;
        void on_error(Id_t client_id, Id_t client_request_id, uint16_t code, std::string_view message, Time_t timestamp) override;

    private:
        void do_accept();
        void remove_connection(Connection* conn);
        void on_message(Connection* from, Message_t message_type, const uint8_t* payload);
        Connection* connect(tcp::socket socket);

        boost::asio::io_context& context_;
        boost::asio::strand<boost::asio::any_io_executor> strand_;
        tcp::acceptor acceptor_;
        std::unordered_map<Id_t, std::unique_ptr<Connection>> clients_;
        std::vector<Connection*> market_data_subscribers_;
        OrderBook order_book_;
        Id_t next_connection_id_;
        Id_t trade_id_;
        Id_t sequence_number_;
        BinaryEventLogger event_logger_;
};
