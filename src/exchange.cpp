#include "exchange.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "time.hpp"

TG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_CON, "CON")

Exchange::Exchange(boost::asio::io_context& context, uint16_t port)
    : context_(context)
    , accept_strand_(context_.get_executor())
    , engine_strand_(context_.get_executor())
    , acceptor_(context_, tcp::endpoint(tcp::v4(), port))
  // , event_logger_(make_timestamped_filename("logs")) 
    {
        order_book_.set_callbacks(this);
        conn_by_id_ = std::make_unique<std::atomic<Connection*>[]>(MAX_CONNECTIONS);
        for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
            conn_by_id_[i].store(nullptr, std::memory_order_relaxed);
        }

    }

Exchange::~Exchange() {
    stop();
}

void Exchange::start() {
    running_.store(true, std::memory_order_release);
    engine_thread_ = std::thread([this] { run_engine_(); });
    boost::asio::dispatch(accept_strand_, [this] { do_accept_(); });
}

void Exchange::stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);

    boost::asio::dispatch(accept_strand_, [this] {
    boost::system::error_code ec;
    acceptor_.close(ec);
    for (auto& [id, state] : clients_) {
        if (state.conn) {
            state.conn->close();
        }
    }
    });

    if (was_running && engine_thread_.joinable()) {
        engine_thread_.join();
    }

    for (size_t i = 0; i < MAX_CONNECTIONS; ++i) {
        conn_by_id_[i].store(nullptr, std::memory_order_relaxed);
    }
    clients_.clear();
    market_data_subscribers_.clear();
}

void Exchange::do_accept_() {
  acceptor_.async_accept(
      boost::asio::bind_executor(
          accept_strand_,
          [this](boost::system::error_code ec, tcp::socket socket) {
            on_accepted_(ec, std::move(socket));
          }
        )
    );
}

void Exchange::on_accepted_(boost::system::error_code ec, tcp::socket socket) {
    if (ec) {
        if (ec == boost::asio::error::operation_aborted) return;
        RLOG(LG_CON, LogLevel::LL_ERROR) << "[Exchange] accept error: " << ec.message();
        if (acceptor_.is_open()) do_accept_();
        return;
    }

    const Id_t id = next_connection_id_++;

    ClientState state;
    state.outbox = std::make_unique<OutboundQueue>();
    state.conn = std::make_unique<Connection>(context_, std::move(socket), id, inbox_, *state.outbox);

    Connection* ptr = state.conn.get();

    ptr->disconnected = [this](Connection* c) {
        InboundMessage m{};
        m.connection_id = c->id();
        m.message_type = static_cast<Message_t>(MessageType::DISCONNECT);
        m.payload_size = 0;
        (void)inbox_.try_push(m); // best-effort; if full, engine is overloaded anyway
    };

    ptr->async_read();
    publish_connection_(id, std::move(state));
    if (acceptor_.is_open()) {
        do_accept_();
    }
}

void Exchange::publish_connection_(Id_t id, ClientState&& state) {
    if (id >= MAX_CONNECTIONS) {
        return;
    }
    Connection* ptr = state.conn.get();
    clients_.emplace(id, std::move(state));
    conn_by_id_[id].store(ptr, std::memory_order_release);
}

void Exchange::run_engine_() {
    InboundMessage msg{};
    while (running_.load(std::memory_order_acquire)) {
        bool did_work = false;
        while (inbox_.try_pop(msg)) {
            did_work = true;
            dispatch_(msg);
        }
        // Simple backoff; refine later (spin/yield/sleep hybrid)
        if (!did_work) { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
    }
}

void Exchange::dispatch_(const InboundMessage& msg) {
  switch (static_cast<MessageType>(msg.message_type)) {
    case MessageType::INSERT_ORDER: {
      const auto* m = reinterpret_cast<const PayloadInsertOrder*>(msg.payload.data());
      order_book_.submit_order(
          m->price,
          m->quantity,
          m->side == Side::BUY,
          msg.connection_id,
          m->client_request_id);
      break;
    }
    case MessageType::CANCEL_ORDER: {
      const auto* m = reinterpret_cast<const PayloadCancelOrder*>(msg.payload.data());
      order_book_.cancel_order(msg.connection_id, m->client_request_id, m->exchange_order_id);
      break;
    }
    case MessageType::AMEND_ORDER: {
      const auto* m = reinterpret_cast<const PayloadAmendOrder*>(msg.payload.data());
      order_book_.amend_order(msg.connection_id, m->client_request_id, m->exchange_order_id, m->new_total_quantity);
      break;
    }
    case MessageType::SUBSCRIBE: {
      subscribe_market_feed_(msg.connection_id);
      break;
    }
    case MessageType::UNSUBSCRIBE: {
      unsubscribe_market_feed_(msg.connection_id);
      break;
    }
    case MessageType::DISCONNECT: {
      remove_connection_(msg.connection_id);
      break;
    }
    default:
      break;
  }
}

Connection* Exchange::conn_ptr_(Id_t id) noexcept {
    if (static_cast<size_t>(id) >= MAX_CONNECTIONS) return nullptr;
    return conn_by_id_[id].load(std::memory_order_acquire);
}

void Exchange::send_to_(Id_t client_id, Message_t message_type, const void* payload) noexcept {
    if (Connection* c = conn_ptr_(client_id)) {
        c->send_message(message_type, payload);
    }
}

void Exchange::broadcast_to_subscribers_(Message_t message_type, const void* payload) noexcept {
    for (Id_t cid : market_data_subscribers_) {
        if (Connection* c = conn_ptr_(cid)) {
            c->send_message(message_type, payload);
        }
    }
}

void Exchange::subscribe_market_feed_(Id_t connection_id) {
  market_data_subscribers_.push_back(connection_id);

  // Snapshot (slow-path, larger than MAX_PAYLOAD_SIZE_BUFFER).
  const Id_t sequence_number = sequence_number_; // share current seq without increment
  std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> bid_volumes;
  std::array<Price_t,  ORDER_BOOK_MESSAGE_DEPTH> bid_prices;
  std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> ask_volumes;
  std::array<Price_t,  ORDER_BOOK_MESSAGE_DEPTH> ask_prices;

  order_book_.build_snapshot(bid_volumes, bid_prices, ask_volumes, ask_prices);

    PayloadOrderBookSnapshot snapshot = make_order_book_snapshot(
        ask_prices, ask_volumes, bid_prices, bid_volumes, sequence_number
    );

  if (Connection* c = conn_ptr_(connection_id)) {
    // Assumed to exist as you specified (unbuffered / bulk path).
    c->send_message_unbuffered(
        static_cast<Message_t>(MessageType::ORDER_BOOK_SNAPSHOT),
        &snapshot,
        static_cast<uint16_t>(sizeof(snapshot))
    );
  }
}

void Exchange::unsubscribe_market_feed_(Id_t connection_id) {
    auto it = std::find(market_data_subscribers_.begin(), market_data_subscribers_.end(), connection_id);
    if (it != market_data_subscribers_.end()) {
        std::swap(*it, market_data_subscribers_.back());
        market_data_subscribers_.pop_back();
    }
}

void Exchange::remove_connection_(Id_t connection_id) {
    unsubscribe_market_feed_(connection_id);

    if (static_cast<size_t>(connection_id) < MAX_CONNECTIONS) {
        conn_by_id_[connection_id].store(nullptr, std::memory_order_release);
    }

    boost::asio::dispatch(accept_strand_, [this, connection_id] {
    auto it = clients_.find(connection_id);
    if (it != clients_.end()) {
        if (it->second.conn) it->second.conn->close();
        clients_.erase(it);
    }
    });
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

    const Id_t trade_id = trade_id_++;
    const Id_t sequence_number = sequence_number_++;

    PayloadPartialFill maker_fill_message = make_partial_fill(
        maker_order.order_id_,
        trade_id,
        price,
        traded_quantity,
        maker_order.quantity_remaining_,
        maker_order.quantity_cumulative_,
        timestamp
    );

    send_to_(maker_order.client_id_, static_cast<Message_t>(MessageType::PARTIAL_FILL_ORDER), &maker_fill_message);

    PayloadPartialFill taker_fill_message = make_partial_fill(
        taker_order_id,
        trade_id,
        price,
        traded_quantity,
        taker_total_quantity - taker_cumulative_quantity,
        taker_cumulative_quantity,
        timestamp
    );

    send_to_(taker_client_id, static_cast<Message_t>(MessageType::PARTIAL_FILL_ORDER), &taker_fill_message);

    PayloadTradeEvent trade_message = make_trade_event(
        sequence_number,
        trade_id,
        price,
        traded_quantity,
        maker_order.is_bid_ ? Side::SELL : Side::BUY,
        timestamp
    );

    broadcast_to_subscribers_(static_cast<Message_t>(MessageType::TRADE_EVENT), &trade_message);
    // event_logger_.log_message(MessageType::TRADE_EVENT, &trade_message);
}

void Exchange::on_order_inserted(Id_t client_request_id, const Order& order, Time_t timestamp) {
    const Id_t sequence_number = sequence_number_++;

    PayloadConfirmOrderInserted confirmation_message = make_confirm_order_inserted(
        client_request_id,
        order.order_id_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        order.price_,
        order.quantity_,
        order.quantity_remaining_,
        timestamp
    );

    send_to_(order.client_id_, static_cast<Message_t>(MessageType::CONFIRM_ORDER_INSERTED), &confirmation_message);

    PayloadOrderInsertedEvent insert_message = make_order_inserted_event(
        sequence_number,
        order.order_id_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        order.price_,
        order.quantity_remaining_,
        timestamp
    );

    broadcast_to_subscribers_(static_cast<Message_t>(MessageType::ORDER_INSERTED_EVENT), &insert_message);
    // event_logger_.log_message(MessageType::ORDER_INSERTED_EVENT, &insert_message);
}

void Exchange::on_order_cancelled(Id_t client_request_id, const Order& order, Time_t timestamp) {
    const Id_t sequence_number = sequence_number_++;

    PayloadConfirmOrderCancelled confirmation_message = make_confirm_order_cancelled(
        client_request_id,
        order.order_id_,
        order.quantity_remaining_,
        order.price_,
        order.is_bid_ ? Side::BUY : Side::SELL,
        timestamp
    );

    send_to_(order.client_id_, static_cast<Message_t>(MessageType::CONFIRM_ORDER_CANCELLED), &confirmation_message);

    PayloadOrderCancelledEvent cancel_message = make_order_cancelled_event(
        sequence_number,
        order.order_id_,
        order.quantity_remaining_,
        timestamp
    );

    broadcast_to_subscribers_(static_cast<Message_t>(MessageType::ORDER_CANCELLED_EVENT), &cancel_message);
    // event_logger_.log_message(MessageType::ORDER_CANCELLED_EVENT, &cancel_message);
}

void Exchange::on_order_amended(Id_t client_request_id, Volume_t quantity_old, const Order& order, Time_t timestamp) {
    const Id_t sequence_number = sequence_number_++;

    PayloadConfirmOrderAmended confirmation_message = make_confirm_order_amended(
        client_request_id,
        order.order_id_,
        quantity_old,
        order.quantity_,
        order.quantity_remaining_,
        timestamp
    );

    send_to_(order.client_id_, static_cast<Message_t>(MessageType::CONFIRM_ORDER_AMENDED), &confirmation_message);

    PayloadOrderAmendedEvent amended_message = make_order_amended_event(
        sequence_number,
        order.order_id_,
        order.quantity_,
        quantity_old,
        timestamp
    );

    broadcast_to_subscribers_(static_cast<Message_t>(MessageType::ORDER_AMENDED_EVENT), &amended_message);
    // event_logger_.log_message(MessageType::ORDER_AMENDED_EVENT, &amended_message);
}

void Exchange::on_level_update(Side side, PriceLevel const& level, Time_t timestamp) {
    const Id_t sequence_number = sequence_number_++;

    PayloadPriceLevelUpdate message = make_price_level_update(
        sequence_number,
        side,
        level.price_,
        level.total_quantity_,
        timestamp
    );

    broadcast_to_subscribers_(static_cast<Message_t>(MessageType::PRICE_LEVEL_UPDATE), &message);
    // event_logger_.log_message(MessageType::PRICE_LEVEL_UPDATE, &message);
}

void Exchange::on_error(Id_t client_id, Id_t client_request_id, uint16_t code, std::string_view message, Time_t timestamp) {
  PayloadError error_message = make_error(client_request_id, code, message, timestamp);
  send_to_(client_id, static_cast<Message_t>(MessageType::ERROR_MSG), &error_message);
}
