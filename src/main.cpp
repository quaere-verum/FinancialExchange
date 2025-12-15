#include "exchange.hpp"
#include "protocol.hpp"
#include "connectivity.hpp"

#include <boost/asio.hpp>
#include <thread>
#include <iostream>

void client_on_message(IConnection* conn, Message_t type, const void* payload) {
    // if (type == static_cast<Message_t>(MessageType::CONFIRM_ORDER_INSERTED)) {
    //     const auto* msg = reinterpret_cast<const PayloadConfirmOrderInserted*>(payload);

    //     std::cout << "=== Confirm Order Inserted ===\n";
    //     std::cout << "Client request ID: " << msg->client_request_id << "\n";
    //     std::cout << "Exchange order ID: " << msg->exchange_order_id << "\n";
    //     std::cout << "Side: " << (msg->side == Side::BUY ? "BUY" : "SELL") << "\n";
    //     std::cout << "Price: " << msg->price << "\n";
    //     std::cout << "Total quantity: " << msg->total_quantity << "\n";
    //     std::cout << "Leaves quantity: " << msg->leaves_quantity << "\n";
    //     std::cout << "Timestamp: " << msg->timestamp << "\n";
    //     std::cout << "==============================\n";
    // } else {
    //     std::cout << "Received message type: " << static_cast<int>(type) << "\n";
    // }
    std::cout << "Receive message type: " << static_cast<int>(type) << "\n";
}

void market_data_on_message(IConnection* conn, Message_t type, const void* payload) {
    if (type == static_cast<Message_t>(MessageType::ORDER_BOOK_SNAPSHOT)) {
        const auto* snap =
            reinterpret_cast<const PayloadOrderBookSnapshot*>(payload);

        std::cout << "\n=== ORDER BOOK SNAPSHOT (seq_nr=" << snap->sequence_number << ") ===\n";
        std::cout << "   BID                ASK\n";
        for (size_t i = 0; i < ORDER_BOOK_MESSAGE_DEPTH; ++i) {
            std::cout
                << snap->bid_volumes[i] << " @ " << snap->bid_prices[i]
                << "    |    "
                << snap->ask_prices[i] << " @ " << snap->ask_volumes[i]
                << "\n";
        }
        std::cout << "===========================\n";
    }
}


using boost::asio::ip::tcp;

int main() {
    try {
        boost::asio::io_context io;

        Exchange exchange(io, 15000);
        exchange.start();

        std::thread io_thread([&]() {
            io.run();
        });

        /* =========================
           CLIENT 1: TRADING CLIENT
           ========================= */

        tcp::socket trading_socket(io);
        trading_socket.connect(tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"), 15000
        ));

        Connection* trading_conn = exchange.connect(std::move(trading_socket));
        trading_conn->message_received = client_on_message;
        trading_conn->async_read();

        PayloadSubscribe trading_sub = make_subscribe(0);
        trading_conn->send_message(
            static_cast<uint8_t>(MessageType::SUBSCRIBE),
            &trading_sub,
            SendMode::ASAP
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        PayloadInsertOrder message1 =
            make_insert_order(1, Side::BUY, 995, 10, Lifespan::FILL_AND_KILL);

        trading_conn->send_message(
            static_cast<uint8_t>(MessageType::INSERT_ORDER),
            &message1,
            SendMode::ASAP
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        PayloadInsertOrder message2 =
            make_insert_order(2, Side::BUY, 994, 7, Lifespan::FILL_AND_KILL);

        trading_conn->send_message(
            static_cast<uint8_t>(MessageType::INSERT_ORDER),
            &message2,
            SendMode::ASAP
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        /* =========================
           CLIENT 2: MARKET DATA
           ========================= */

        tcp::socket md_socket(io);
        md_socket.connect(tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"), 15000
        ));

        Connection* md_conn = exchange.connect(std::move(md_socket));
        md_conn->message_received = market_data_on_message;
        md_conn->async_read();

        PayloadSubscribe md_sub = make_subscribe(0);
        md_conn->send_message(
            static_cast<uint8_t>(MessageType::SUBSCRIBE),
            &md_sub,
            SendMode::ASAP
        );

        // Give time for snapshot to arrive
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        exchange.stop();
        io.stop();
        io_thread.join();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
}
