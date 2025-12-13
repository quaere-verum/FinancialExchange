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
    std::cout << "Received message type: " << static_cast<int>(type) << "\n";
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

        tcp::socket client_socket(io);

        client_socket.connect(tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"), 15000
        ));

        Connection* connection = exchange.connect(std::move(client_socket));
        connection->message_received = client_on_message;
        connection->async_read();

        PayloadSubscribe subscription = make_subscribe(0);

        connection->send_message(
            static_cast<uint8_t>(MessageType::SUBSCRIBE),
            &subscription,
            SendMode::ASAP
        );

        std::cout << "Subscription request sent.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        PayloadInsertOrder message1 =
            make_insert_order(1, Side::BUY, 995, 10, Lifespan::FILL_AND_KILL);

        connection->send_message(
            static_cast<uint8_t>(MessageType::INSERT_ORDER),
            &message1,
            SendMode::ASAP
        );

        std::cout << "Test message 1 sent.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        exchange.print_book();

        PayloadInsertOrder message2 =
            make_insert_order(2, Side::BUY, 994, 7, Lifespan::FILL_AND_KILL);

        connection->send_message(
            static_cast<uint8_t>(MessageType::INSERT_ORDER),
            &message2,
            SendMode::ASAP
        );

        std::cout << "Test message 2 sent.\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        exchange.print_book();

        exchange.stop();   // closes client connections cleanly

        io.stop();         // drains all pending handlers
        io_thread.join();  // wait for io_context thread to finish
    }
    catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
