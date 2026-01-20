#pragma once
#include "exchange.hpp"
#include "time.hpp"
#include "logging.hpp"

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")

Exchange::Exchange(boost::asio::io_context& context, uint16_t port)
    : context_(context),
    strand_(context_.get_executor()),
    acceptor_(context, tcp::endpoint(tcp::v4(), port)), 
    next_connection_id_(0),
    trade_id_(0),
    sequence_number_(0),
    event_logger_(make_timestamped_filename("logs")) {
        order_book_.set_callbacks(this);
    }

void Exchange::start() {
    acceptor_.async_accept(
        boost::asio::bind_executor(
            strand_,
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    connect(std::move(socket));
                } else if (ec == boost::asio::error::operation_aborted) {
                    return;
                }
                if (acceptor_.is_open()) {do_accept();}
            }
        )
    );
}

void Exchange::stop() {
    boost::asio::dispatch(
        strand_,
        [this]() {
            boost::system::error_code ec;
            acceptor_.close(ec);

            for (auto& [id, client] : clients_) {
                if (client) {
                    client->close();
                }
            }
            clients_.clear();
        }
    );
}


void Exchange::broadcast(Message_t message_type, const uint8_t* payload) {
    boost::asio::post(
        strand_,
        [this, message_type, payload]() {
            for (auto& [id, client] : clients_) {
                if (client) {
                    client->send_message(message_type, payload);
                }
            }
        }
    );
}

void Exchange::send_to(Connection* client, Message_t message_type, const void* payload) {
    if (client) {client->send_message(message_type, payload);}
}

void Exchange::do_accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (!ec) {Connection* client = connect(std::move(socket));}
            do_accept();
        }
    );
}

Connection* Exchange::connect(tcp::socket socket) {
    Id_t id = next_connection_id_++;
    auto connection = std::make_unique<Connection>(context_, std::move(socket), id, &strand_);
    Connection* ptr = connection.get();
    ptr->message_received = [this](Connection* from, Message_t type, const uint8_t* data){
        this->on_message(from, type, data);
    };
    ptr->disconnected = [this](Connection* c) { this->remove_connection(static_cast<Connection*>(c)); };
    ptr->async_read();
    clients_.emplace(id, std::move(connection));
    return ptr;
}

void Exchange::subscribe_market_feed(Connection* client) {
    Id_t sequence_number = sequence_number_; // Do not increment, simply share the latest sequence_number_ with the client
    market_data_subscribers_.push_back(client);

    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> bid_volumes;
    std::array<Price_t,  ORDER_BOOK_MESSAGE_DEPTH> bid_prices;
    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> ask_volumes;
    std::array<Price_t,  ORDER_BOOK_MESSAGE_DEPTH> ask_prices;

    order_book_.build_snapshot(bid_volumes, bid_prices, ask_volumes, ask_prices);

    PayloadOrderBookSnapshot snapshot = make_order_book_snapshot(
        ask_prices,
        ask_volumes,
        bid_prices,
        bid_volumes,
        sequence_number
    );
    send_to(
        client,
        static_cast<Message_t>(MessageType::ORDER_BOOK_SNAPSHOT),
        &snapshot
    );
}

void Exchange::unsubscribe_market_feed(Connection* client) {
    auto it = std::find(market_data_subscribers_.begin(), market_data_subscribers_.end(), client);
    if (it != market_data_subscribers_.end()) {
        std::swap(*it, market_data_subscribers_.back());
        market_data_subscribers_.pop_back();
    }
}

void Exchange::remove_connection(Connection* connection) {
    unsubscribe_market_feed(connection);
    clients_.erase(connection->id());
}

void Exchange::on_trade(
    const Order& maker_order,
    Id_t taker_client_id,
    Id_t taker_order_id,
    Price_t price,
    Volume_t taker_total_quantity,
    Volume_t taker_cumulative_quantity,
    Volume_t traded_quantity,
    Time_t timestamp
) {
    assert(strand_.running_in_this_thread());
    Id_t trade_id = trade_id_++;
    Id_t sequence_number = sequence_number_++;
    PayloadPartialFill maker_fill_message = make_partial_fill(
        maker_order.order_id_,
        trade_id,
        price,
        traded_quantity,
        maker_order.quantity_remaining_,
        maker_order.quantity_cumulative_,
        timestamp
    );

    auto maker_client = clients_.find(maker_order.client_id_);
    if (maker_client != clients_.end() && maker_client->second) {
        auto maker_ptr = maker_client->second.get();
        send_to(
            maker_ptr, 
            static_cast<Message_t>(MessageType::PARTIAL_FILL_ORDER), 
            &maker_fill_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent partial fill confirmation to " << maker_ptr->id(); 
    }
    PayloadPartialFill taker_fill_message = make_partial_fill(
        taker_order_id,
        trade_id,
        price,
        traded_quantity,
        taker_total_quantity - taker_cumulative_quantity,
        taker_cumulative_quantity,
        timestamp
    );

    auto taker_client = clients_.find(taker_client_id);
    if (taker_client != clients_.end() && taker_client->second) {
        auto taker_ptr = taker_client->second.get();
        send_to(
            taker_ptr, 
            static_cast<Message_t>(MessageType::PARTIAL_FILL_ORDER), 
            &taker_fill_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent partial fill confirmation to " << taker_ptr->id(); 
    }

    PayloadTradeEvent trade_message = make_trade_event(
        sequence_number,
        trade_id,
        price,
        traded_quantity,
        maker_order.is_bid_ ? Side::SELL : Side::BUY,
        timestamp
    );
    for (Connection* c : market_data_subscribers_) {
        if (c) {
            send_to(
                c,
                static_cast<Message_t>(MessageType::TRADE_EVENT),
                &trade_message
            );
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent trade event to " << c->id(); 
        }
    }

    event_logger_.log_message(MessageType::TRADE_EVENT, &trade_message);
}

void Exchange::on_order_inserted(Id_t client_request_id, const Order& order, Time_t timestamp) {
    assert(strand_.running_in_this_thread());
    Id_t sequence_number = sequence_number_++;

    RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] on_order_inserted() client_request_id=" << client_request_id;
    PayloadConfirmOrderInserted confirmation_message = make_confirm_order_inserted(
        client_request_id,
        order.order_id_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        order.price_,
        order.quantity_,
        order.quantity_remaining_,
        timestamp
    );

    auto client = clients_.find(order.client_id_);
    if (client != clients_.end() && client->second) {
        auto client_ptr = client->second.get();
        send_to(
            client_ptr, 
            static_cast<Message_t>(MessageType::CONFIRM_ORDER_INSERTED), 
            &confirmation_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent insertion confirmation to " << client_ptr->id(); 
    }

    PayloadOrderInsertedEvent insert_message = make_order_inserted_event(
        sequence_number,
        order.order_id_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        order.price_,
        order.quantity_remaining_,
        timestamp
    );
    for (Connection* c : market_data_subscribers_) {
        if (c) {
            send_to(
                c,
                static_cast<Message_t>(MessageType::ORDER_INSERTED_EVENT),
                &insert_message
            );
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent insertion event to " << c->id(); 
        }
    }
    event_logger_.log_message(MessageType::ORDER_INSERTED_EVENT, &insert_message);
}

void Exchange::on_order_cancelled(Id_t client_request_id, const Order& order, Time_t timestamp) {
    assert(strand_.running_in_this_thread());
    Id_t sequence_number = sequence_number_++;

    PayloadConfirmOrderCancelled confirmation_message = make_confirm_order_cancelled(
        client_request_id,
        order.order_id_,
        order.quantity_remaining_,
        order.price_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        timestamp
    );

    auto client = clients_.find(order.client_id_);
    if (client != clients_.end() && client->second) {
        auto client_ptr = client->second.get();
        send_to(
            client_ptr, 
            static_cast<Message_t>(MessageType::CONFIRM_ORDER_CANCELLED), 
            &confirmation_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent cancel confirmation to " << client_ptr->id(); 
    }

    PayloadOrderCancelledEvent cancel_message = make_order_cancelled_event(
        sequence_number,
        order.order_id_,
        order.quantity_remaining_,
        timestamp
    );
    for (Connection* c : market_data_subscribers_) {
        if (c) {
            send_to(
                c,
                static_cast<Message_t>(MessageType::ORDER_CANCELLED_EVENT),
                &cancel_message
            );
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent cancel event to " << c->id(); 
        }
    }
    event_logger_.log_message(MessageType::ORDER_CANCELLED_EVENT, &cancel_message);
}


void Exchange::on_order_amended(Id_t client_request_id, Volume_t quantity_old, const Order& order, Time_t timestamp) {
    assert(strand_.running_in_this_thread());
    Id_t sequence_number = sequence_number_++;

    PayloadConfirmOrderAmended confirmation_message = make_confirm_order_amended(
        client_request_id,
        order.order_id_,
        quantity_old,
        order.quantity_,
        order.quantity_remaining_,
        timestamp
    );

    auto client = clients_.find(order.client_id_);
    if (client != clients_.end() && client->second) {
        auto client_ptr = client->second.get();
        send_to(
            client_ptr, 
            static_cast<Message_t>(MessageType::CONFIRM_ORDER_AMENDED), 
            &confirmation_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent amendment confirmation to " << client_ptr->id(); 
    }

    PayloadOrderAmendedEvent amended_message = make_order_amended_event(
        sequence_number,
        order.order_id_,
        order.quantity_,
        quantity_old,
        timestamp
    );
    for (Connection* c : market_data_subscribers_) {
        if (c) {
            send_to(
                c,
                static_cast<Message_t>(MessageType::ORDER_AMENDED_EVENT),
                &amended_message
            );
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent amendment event to " << c->id(); 
        }
    }
    event_logger_.log_message(MessageType::ORDER_AMENDED_EVENT, &amended_message);
}

void Exchange::on_level_update(Side side, PriceLevel const& level, Time_t timestamp) {
    assert(strand_.running_in_this_thread());
    Id_t sequence_number = sequence_number_++;
    PayloadPriceLevelUpdate message = make_price_level_update(
        sequence_number,
        side,
        level.price_,
        level.total_quantity_,
        timestamp
    );
    for (Connection* c : market_data_subscribers_) {
        if (c) {
            send_to(
                c,
                static_cast<Message_t>(MessageType::PRICE_LEVEL_UPDATE),
                &message
            );
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent level update to " << c->id(); 
        }
    }
    event_logger_.log_message(MessageType::PRICE_LEVEL_UPDATE, &message);
}

void Exchange::on_error(Id_t client_id, Id_t client_request_id, uint16_t code, std::string_view message, Time_t timestamp) {
    assert(strand_.running_in_this_thread());
    PayloadError error_message = make_error(client_request_id, code, message, timestamp);
    auto client = clients_.find(client_id);
    if (client != clients_.end() && client->second) {
        auto client_ptr = client->second.get();
        send_to(
            client_ptr, 
            static_cast<Message_t>(MessageType::ERROR_MSG), 
            &error_message
        );
        RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Sent error message(" << message << ") to " << client_ptr->id(); 
    }
}

void Exchange::on_message(Connection* from, Message_t message_type, const uint8_t* payload) {
    assert(strand_.running_in_this_thread());
    switch (static_cast<MessageType>(message_type)) {
        case MessageType::INSERT_ORDER: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received insert order from " << from->id(); 
            const PayloadInsertOrder* message = reinterpret_cast<const PayloadInsertOrder*>(payload);
            order_book_.submit_order(
                message->price,
                message->quantity,
                message->side == Side::BUY, // TODO: add lifespan
                from->id(),
                message->client_request_id
            );
            break;
        } case MessageType::CANCEL_ORDER: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received cancel order from " << from->id();
            const PayloadCancelOrder* message = reinterpret_cast<const PayloadCancelOrder*>(payload);
            order_book_.cancel_order(
                from->id(),
                message->client_request_id,
                message->exchange_order_id
            );
            break;
        } case MessageType::AMEND_ORDER: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received amend request from " << from->id();
            const PayloadAmendOrder* message = reinterpret_cast<const PayloadAmendOrder*>(payload);
            order_book_.amend_order(from->id(), message->client_request_id, message->exchange_order_id, message->new_total_quantity);
            break;
        } case MessageType::SUBSCRIBE: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received subscribe request from " << from->id();
            subscribe_market_feed(from);
            break;
        } case MessageType::UNSUBSCRIBE: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received unsubscribe from " << from->id();
            unsubscribe_market_feed(from);
            break;
        } case MessageType::DISCONNECT: {
            RLOG(LG_CON, LogLevel::LL_DEBUG) << "[Exchange] Received disconnect request from " << from->id();
            remove_connection(from);
            break;
        }
    }
}
