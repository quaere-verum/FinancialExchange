// exchange.hpp
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "binary_logger.hpp"
#include "connectivity.hpp"
#include "types.hpp"
#include "protocol.hpp"
#include "order_book.hpp"
#include "callbacks.hpp"
#include "logging.hpp"
#include "connectivity.hpp"

constexpr size_t MAX_CONNECTIONS = 1 << 5;

class Exchange final : public OrderBookCallbacks {
    public:
    using tcp = boost::asio::ip::tcp;

    Exchange(boost::asio::io_context& context, uint16_t port);
    ~Exchange();

    void start();
    void stop();

    void print_book() { order_book_.print_book(); }

    void on_trade(
        const Order& maker_order,
        Id_t taker_client_id,
        Id_t taker_order_id,
        Price_t price,
        Volume_t taker_total_quantity,
        Volume_t taker_cumulative_quantity,
        Volume_t traded_quantity,
        Time_t timestamp) override;

    void on_order_inserted(Id_t client_request_id, const Order& order, Time_t timestamp) override;
    void on_order_cancelled(Id_t client_request_id, const Order& order, Time_t timestamp) override;
    void on_order_amended(Id_t client_request_id, Volume_t quantity_old, const Order& order, Time_t timestamp) override;
    void on_level_update(Side side, PriceLevel const& level, Time_t timestamp) override;
    void on_error(Id_t client_id, Id_t client_request_id, uint16_t code, std::string_view message, Time_t timestamp) override;

    private:
    struct ClientState {
        std::unique_ptr<OutboundQueue> outbox;
        std::unique_ptr<Connection> conn;
    };

    private:
    void do_accept_();
    void on_accepted_(boost::system::error_code ec, tcp::socket socket);
    void publish_connection_(Id_t id, ClientState&& st);

    void run_engine_();
    void dispatch_(const InboundMessage& msg);

    void subscribe_market_feed_(Id_t connection_id);
    void unsubscribe_market_feed_(Id_t connection_id);
    void remove_connection_(Id_t connection_id);

    inline Connection* conn_ptr_(Id_t id) noexcept;
    inline void send_to_(Id_t client_id, Message_t message_type, const void* payload) noexcept;
    inline void broadcast_to_subscribers_(Message_t message_type, const void* payload) noexcept;

    private:
    boost::asio::io_context& context_;
    boost::asio::strand<boost::asio::any_io_executor> accept_strand_;
    boost::asio::strand<boost::asio::any_io_executor> engine_strand_;
    tcp::acceptor acceptor_;

    InboundQueue inbox_;

    std::thread engine_thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<Id_t, ClientState> clients_;

    std::unique_ptr<std::atomic<Connection*>[]> conn_by_id_;

    std::vector<Id_t> market_data_subscribers_;

    OrderBook order_book_;

    Id_t next_connection_id_{0};
    Id_t trade_id_{0};
    Id_t sequence_number_{0};

    // BinaryEventLogger event_logger_;
};
