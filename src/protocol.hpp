#pragma once
#include <cstring>
#include <array>
#include <string>
#include "types.hpp"

enum class MessageType : Message_t {
    CONNECT = 1,
    DISCONNECT = 2,
    INSERT_ORDER = 3,
    CANCEL_ORDER = 4,
    AMEND_ORDER = 5,
    SUBSCRIBE = 6,
    UNSUBSCRIBE = 7,
    ORDER_STATUS_REQUEST = 8,

    CONFIRM_CONNECTED = 11,
    CONFIRM_ORDER_INSERTED = 12,
    CONFIRM_ORDER_CANCELLED = 13,
    CONFIRM_ORDER_AMENDED = 14,
    PARTIAL_FILL_ORDER = 15,
    ORDER_STATUS = 16,
    ERROR_MSG = 17,

    ORDER_BOOK_SNAPSHOT = 21,
    TRADE_EVENT = 23,
    ORDER_INSERTED_EVENT = 24,
    ORDER_CANCELLED_EVENT = 25,
    ORDER_AMENDED_EVENT = 26,
    PRICE_LEVEL_UPDATE = 27
};

#pragma pack(push, 1)

struct MessageHeader {
    MessageType type;
    uint16_t size;
};

struct PayloadDisconnect {
    Id_t client_request_id;
};

struct PayloadInsertOrder {
    Id_t client_request_id;
    Side side;
    Price_t price;
    Volume_t quantity;
    Lifespan lifespan;
};

struct PayloadCancelOrder {
    Id_t client_request_id;
    Id_t exchange_order_id;
};

struct PayloadAmendOrder {
    Id_t client_request_id;
    Id_t exchange_order_id;
    Volume_t new_total_quantity;
};

struct PayloadSubscribe {
    Id_t client_request_id;
};

struct PayloadUnsubscribe {
    Id_t client_request_id;
};

struct PayloadOrderStatusRequest {
    Id_t client_request_id;
    Id_t exchange_order_id;
};

struct PayloadError {
    Id_t client_request_id;
    uint16_t code;
    char message[ERROR_TEXT_LEN];
    Time_t timestamp;
};

struct PayloadConfirmOrderInserted {
    Id_t client_request_id;
    Id_t exchange_order_id;
    Side side;
    Price_t price;
    Volume_t total_quantity;
    Volume_t leaves_quantity;
    Time_t timestamp;
};

struct PayloadConfirmOrderCancelled {
    Id_t client_request_id;
    Id_t exchange_order_id;
    Volume_t leaves_quantity;
    Price_t price;
    Side side;
    Time_t timestamp;
};

struct PayloadConfirmOrderAmended {
    Id_t client_request_id;
    Id_t exchange_order_id;
    Volume_t old_total_quantity;
    Volume_t new_total_quantity;
    Volume_t leaves_quantity;
    Time_t timestamp;
};

struct PayloadPartialFill {
    Id_t exchange_order_id;
    Id_t trade_id;
    Price_t last_price;
    Volume_t last_quantity;
    Volume_t leaves_quantity;
    Volume_t cumulative_quantity;
    Time_t timestamp;
};

struct PayloadOrderStatus {
    Id_t client_request_id;
    Id_t exchange_order_id;
    Side side;
    Price_t limit_price;
    Price_t last_price;
    Volume_t total_quantity;
    Volume_t filled_quantity;
    Volume_t leaves_quantity;
    Time_t timestamp;
};

struct PayloadOrderBookSnapshot {
    std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH> ask_prices;
    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> ask_volumes;
    std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH> bid_prices;
    std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH> bid_volumes;
    Id_t sequence_number;
};

struct PayloadTradeEvent {
    Id_t sequence_number;
    Id_t trade_id;
    Price_t price;
    Volume_t quantity;
    Side taker_side;
    Time_t timestamp;
};

struct PayloadOrderInsertedEvent {
    Id_t sequence_number;
    Id_t order_id;
    Side side;
    Price_t price;
    Volume_t quantity;
    Time_t timestamp;
};

struct PayloadOrderCancelledEvent {
    Id_t sequence_number;
    Id_t order_id;
    Volume_t remaining_quantity;
    Time_t timestamp;
};

struct PayloadOrderAmendedEvent {
    Id_t sequence_number;
    Id_t order_id;
    Volume_t quantity_new;
    Volume_t quantity_old;
    Time_t timestamp;
};

struct PayloadPriceLevelUpdate {
    Id_t sequence_number;
    Side side;
    Price_t price;
    Volume_t total_volume;
    Time_t timestamp;
};


#pragma pack(pop)

constexpr size_t MAX_PAYLOAD_SIZE = []() {
    size_t sizes[] = {
        sizeof(PayloadDisconnect),
        sizeof(PayloadInsertOrder),
        sizeof(PayloadCancelOrder),
        sizeof(PayloadAmendOrder),
        sizeof(PayloadSubscribe),
        sizeof(PayloadUnsubscribe),
        sizeof(PayloadOrderStatusRequest),
        sizeof(PayloadError),
        sizeof(PayloadConfirmOrderInserted),
        sizeof(PayloadConfirmOrderCancelled),
        sizeof(PayloadConfirmOrderAmended),
        sizeof(PayloadPartialFill),
        sizeof(PayloadOrderStatus),
        sizeof(PayloadOrderBookSnapshot),
        sizeof(PayloadTradeEvent),
        sizeof(PayloadOrderInsertedEvent),
        sizeof(PayloadOrderCancelledEvent),
        sizeof(PayloadOrderAmendedEvent),
        sizeof(PayloadPriceLevelUpdate)
    };
    size_t m = 0;
    for (size_t s : sizes) if (s > m) m = s;
    return m;
}();

// Excludes PayloadOrderBookSnapshot because it's a large struct which won't enter the SPSC (or MPSC) queue
constexpr size_t MAX_PAYLOAD_SIZE_BUFFER = []() {
    size_t sizes[] = {
        sizeof(PayloadDisconnect),
        sizeof(PayloadInsertOrder),
        sizeof(PayloadCancelOrder),
        sizeof(PayloadAmendOrder),
        sizeof(PayloadSubscribe),
        sizeof(PayloadUnsubscribe),
        sizeof(PayloadError),
        sizeof(PayloadConfirmOrderInserted),
        sizeof(PayloadConfirmOrderCancelled),
        sizeof(PayloadConfirmOrderAmended),
        sizeof(PayloadPartialFill),
        sizeof(PayloadTradeEvent),
        sizeof(PayloadOrderInsertedEvent),
        sizeof(PayloadOrderCancelledEvent),
        sizeof(PayloadOrderAmendedEvent),
        sizeof(PayloadPriceLevelUpdate)
    };
    size_t m = 0;
    for (size_t s : sizes) if (s > m) m = s;
    return m;
}();

inline size_t payload_size_for_type(MessageType t) {
    switch (t) {
        case MessageType::DISCONNECT: return sizeof(PayloadDisconnect);
        case MessageType::INSERT_ORDER: return sizeof(PayloadInsertOrder);
        case MessageType::CANCEL_ORDER: return sizeof(PayloadCancelOrder);
        case MessageType::AMEND_ORDER: return sizeof(PayloadAmendOrder);
        case MessageType::SUBSCRIBE: return sizeof(PayloadSubscribe);
        case MessageType::UNSUBSCRIBE: return sizeof(PayloadUnsubscribe);
        case MessageType::ORDER_STATUS_REQUEST: return sizeof(PayloadOrderStatusRequest);
        case MessageType::ERROR_MSG: return sizeof(PayloadError);

        case MessageType::CONFIRM_ORDER_INSERTED: return sizeof(PayloadConfirmOrderInserted);
        case MessageType::CONFIRM_ORDER_CANCELLED: return sizeof(PayloadConfirmOrderCancelled);
        case MessageType::CONFIRM_ORDER_AMENDED: return sizeof(PayloadConfirmOrderAmended);
        case MessageType::PARTIAL_FILL_ORDER: return sizeof(PayloadPartialFill);
        case MessageType::ORDER_STATUS: return sizeof(PayloadOrderStatus);

        case MessageType::ORDER_BOOK_SNAPSHOT: return sizeof(PayloadOrderBookSnapshot);
        case MessageType::TRADE_EVENT: return sizeof(PayloadTradeEvent);
        case MessageType::ORDER_INSERTED_EVENT: return sizeof(PayloadOrderInsertedEvent);
        case MessageType::ORDER_CANCELLED_EVENT: return sizeof(PayloadOrderCancelledEvent);
        case MessageType::ORDER_AMENDED_EVENT: return sizeof(PayloadOrderAmendedEvent);
        case MessageType::PRICE_LEVEL_UPDATE: return sizeof(PayloadPriceLevelUpdate);

        default: return 0;
    }
}

inline size_t write_message_to_buffer(uint8_t* buf, size_t buf_len, MessageType t, const void* payload) {
    auto psize = payload_size_for_type(t);
    if (buf_len < 1 + psize) return 0; // insufficient
    buf[0] = static_cast<uint8_t>(t);
    std::memcpy(buf + 1, payload, psize);
    return 1 + psize;
}

inline bool parse_message(const uint8_t* buf, size_t len, MessageType& out_type, const uint8_t*& out_payload) {
    if (len < 1) return false;
    MessageType t = static_cast<MessageType>(buf[0]);
    size_t psize = payload_size_for_type(t);
    if (psize == 0) return false;
    if (len < 1 + psize) return false;
    out_type = t;
    out_payload = buf + 1;
    return true;
}

inline bool parse_message_full(const uint8_t* buf, size_t len, MessageType& out_type, const void*& out_struct) {
    const uint8_t* payload_ptr;
    if (!parse_message(buf, len, out_type, payload_ptr))
        return false;

    switch (out_type) {
        case MessageType::DISCONNECT:
            out_struct = reinterpret_cast<const PayloadDisconnect*>(payload_ptr);
            return true;

        case MessageType::INSERT_ORDER:
            out_struct = reinterpret_cast<const PayloadInsertOrder*>(payload_ptr);
            return true;

        case MessageType::CANCEL_ORDER:
            out_struct = reinterpret_cast<const PayloadCancelOrder*>(payload_ptr);
            return true;

        case MessageType::AMEND_ORDER:
            out_struct = reinterpret_cast<const PayloadAmendOrder*>(payload_ptr);
            return true;

        case MessageType::SUBSCRIBE:
            out_struct = reinterpret_cast<const PayloadSubscribe*>(payload_ptr);
            return true;

        case MessageType::UNSUBSCRIBE:
            out_struct = reinterpret_cast<const PayloadUnsubscribe*>(payload_ptr);
            return true;

        case MessageType::ORDER_STATUS_REQUEST:
            out_struct = reinterpret_cast<const PayloadOrderStatusRequest*>(payload_ptr);
            return true;

        case MessageType::ERROR_MSG:
            out_struct = reinterpret_cast<const PayloadError*>(payload_ptr);
            return true;

        case MessageType::CONFIRM_ORDER_INSERTED:
            out_struct = reinterpret_cast<const PayloadConfirmOrderInserted*>(payload_ptr);
            return true;

        case MessageType::CONFIRM_ORDER_CANCELLED:
            out_struct = reinterpret_cast<const PayloadConfirmOrderCancelled*>(payload_ptr);
            return true;

        case MessageType::CONFIRM_ORDER_AMENDED:
            out_struct = reinterpret_cast<const PayloadConfirmOrderAmended*>(payload_ptr);
            return true;

        case MessageType::PARTIAL_FILL_ORDER:
            out_struct = reinterpret_cast<const PayloadPartialFill*>(payload_ptr);
            return true;

        case MessageType::ORDER_STATUS:
            out_struct = reinterpret_cast<const PayloadOrderStatus*>(payload_ptr);
            return true;

        case MessageType::ORDER_BOOK_SNAPSHOT:
            out_struct = reinterpret_cast<const PayloadOrderBookSnapshot*>(payload_ptr);
            return true;

        case MessageType::TRADE_EVENT:
            out_struct = reinterpret_cast<const PayloadTradeEvent*>(payload_ptr);
            return true;

        case MessageType::ORDER_INSERTED_EVENT:
            out_struct = reinterpret_cast<const PayloadOrderInsertedEvent*>(payload_ptr);
            return true;

        case MessageType::ORDER_CANCELLED_EVENT:
            out_struct = reinterpret_cast<const PayloadOrderCancelledEvent*>(payload_ptr);
            return true;

        case MessageType::ORDER_AMENDED_EVENT:
            out_struct = reinterpret_cast<const PayloadOrderAmendedEvent*>(payload_ptr);
            return true;
        
        default:
            return false;
    }
}

inline PayloadDisconnect make_disconnect(Id_t client_request_id) {
    PayloadDisconnect p{};
    p.client_request_id = client_request_id;
    return p;
}

inline PayloadSubscribe make_subscribe(Id_t client_request_id) {
    PayloadSubscribe p{};
    p.client_request_id = client_request_id;
    return p;
}

inline PayloadUnsubscribe make_unsubscribe(Id_t client_request_id) {
    PayloadUnsubscribe p{};
    p.client_request_id = client_request_id;
    return p;
}

inline PayloadInsertOrder make_insert_order(
    Id_t client_request_id,
    Side side,
    Price_t price,
    Volume_t quantity,
    Lifespan lifespan
) {
    PayloadInsertOrder p{};
    p.client_request_id = client_request_id;
    p.side = side;
    p.price = price;
    p.quantity = quantity;
    p.lifespan = lifespan;
    return p;
}

inline PayloadCancelOrder make_cancel_order(
    Id_t client_request_id,
    Id_t exchange_order_id
) {
    PayloadCancelOrder p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    return p;
}

inline PayloadAmendOrder make_amend_order(
    Id_t client_request_id,
    Id_t exchange_order_id,
    Volume_t new_total_quantity
) {
    PayloadAmendOrder p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    p.new_total_quantity = new_total_quantity;
    return p;
}

inline PayloadOrderStatusRequest make_order_status_request(
    Id_t client_request_id,
    Id_t exchange_order_id
) {
    PayloadOrderStatusRequest p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    return p;
}

inline PayloadError make_error(
    Id_t client_request_id,
    uint16_t code,
    std::string_view message,
    Time_t timestamp
) {
    PayloadError p{};
    p.client_request_id = client_request_id;
    p.code = code;
    p.timestamp = timestamp;

    std::size_t n = std::min<std::size_t>(ERROR_TEXT_LEN - 1, message.size());
    std::memcpy(p.message, message.data(), n);
    p.message[n] = '\0';

    return p;
}

inline PayloadConfirmOrderInserted make_confirm_order_inserted(
    Id_t client_request_id,
    Id_t exchange_order_id,
    Side side,
    Price_t price,
    Volume_t total_quantity,
    Volume_t leaves_quantity,
    Time_t timestamp
) {
    PayloadConfirmOrderInserted p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    p.side = side;
    p.price = price;
    p.total_quantity = total_quantity;
    p.leaves_quantity = leaves_quantity;
    p.timestamp = timestamp;
    return p;
}

inline PayloadConfirmOrderCancelled make_confirm_order_cancelled(
    Id_t client_request_id,
    Id_t exchange_order_id,
    Volume_t leaves_quantity,
    Price_t price,
    Side side,
    Time_t timestamp
) {
    PayloadConfirmOrderCancelled p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    p.leaves_quantity = leaves_quantity;
    p.price = price;
    p.side = side;
    p.timestamp = timestamp;
    return p;
}

inline PayloadConfirmOrderAmended make_confirm_order_amended(
    Id_t client_request_id,
    Id_t exchange_order_id,
    Volume_t old_total_quantity,
    Volume_t new_total_quantity,
    Volume_t leaves_quantity,
    Time_t timestamp
) {
    PayloadConfirmOrderAmended p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    p.old_total_quantity = old_total_quantity;
    p.new_total_quantity = new_total_quantity;
    p.leaves_quantity = leaves_quantity;
    p.timestamp = timestamp;
    return p;
}

inline PayloadPartialFill make_partial_fill(
    Id_t exchange_order_id,
    Id_t trade_id,
    Price_t last_price,
    Volume_t last_quantity,
    Volume_t leaves_quantity,
    Volume_t cumulative_quantity,
    Time_t timestamp
) {
    PayloadPartialFill p{};
    p.exchange_order_id = exchange_order_id;
    p.trade_id = trade_id;
    p.last_price = last_price;
    p.last_quantity = last_quantity;
    p.leaves_quantity = leaves_quantity;
    p.cumulative_quantity = cumulative_quantity;
    p.timestamp = timestamp;
    return p;
}

inline PayloadOrderStatus make_order_status(
    Id_t client_request_id,
    Id_t exchange_order_id,
    Side side,
    Volume_t total_quantity,
    Volume_t filled_quantity,
    Volume_t leaves_quantity,
    Price_t limit_price,
    Price_t last_price,
    Time_t timestamp
) {
    PayloadOrderStatus p{};
    p.client_request_id = client_request_id;
    p.exchange_order_id = exchange_order_id;
    p.side = side;
    p.total_quantity = total_quantity;
    p.filled_quantity = filled_quantity;
    p.leaves_quantity = leaves_quantity;
    p.limit_price = limit_price;
    p.last_price = last_price;
    p.timestamp = timestamp;
    return p;
}

inline PayloadOrderBookSnapshot make_order_book_snapshot(
    const std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_prices,
    const std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& ask_volumes,
    const std::array<Price_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_prices,
    const std::array<Volume_t, ORDER_BOOK_MESSAGE_DEPTH>& bid_volumes,
    Id_t sequence_number
) {
    PayloadOrderBookSnapshot p{};
    p.ask_prices = ask_prices;
    p.ask_volumes = ask_volumes;
    p.bid_prices = bid_prices;
    p.bid_volumes = bid_volumes;
    p.sequence_number = sequence_number;
    return p;
}

inline PayloadTradeEvent make_trade_event(
    Id_t sequence_number,
    Id_t trade_id,
    Price_t price,
    Volume_t quantity,
    Side taker_side,
    Time_t timestamp
) {
    PayloadTradeEvent p{};
    p.sequence_number = sequence_number;
    p.trade_id = trade_id;
    p.price = price;
    p.quantity = quantity;
    p.taker_side = taker_side;
    p.timestamp = timestamp;
    return p;
}

inline PayloadOrderInsertedEvent make_order_inserted_event(
    Id_t sequence_number,
    Id_t order_id,
    Side side,
    Price_t price,
    Volume_t quantity,
    Time_t timestamp
) {
    PayloadOrderInsertedEvent p{};
    p.sequence_number = sequence_number;
    p.order_id = order_id;
    p.side = side;
    p.price = price;
    p.quantity = quantity;
    p.timestamp = timestamp;
    return p;
}

inline PayloadOrderCancelledEvent  make_order_cancelled_event(
    Id_t sequence_number,
    Id_t order_id,
    Volume_t remaining_quantity,
    Time_t timestamp
) {
    PayloadOrderCancelledEvent p{};
    p.sequence_number = sequence_number;
    p.order_id = order_id;
    p.remaining_quantity = remaining_quantity;
    p.timestamp = timestamp;
    return p;
}

inline PayloadOrderAmendedEvent make_order_amended_event(
    Id_t sequence_number,
    Id_t order_id,
    Volume_t quantity_new,
    Volume_t quantity_old,
    Time_t timestamp
) {
    PayloadOrderAmendedEvent p{};
    p.sequence_number = sequence_number;
    p.order_id = order_id;
    p.quantity_new = quantity_new;
    p.quantity_old = quantity_old;
    p.timestamp = timestamp;
    return p;
}

inline PayloadPriceLevelUpdate make_price_level_update(
    Id_t sequence_number,
    Side side,
    Price_t price,
    Volume_t total_volume,
    Time_t timestamp
) {
    PayloadPriceLevelUpdate p{};
    p.sequence_number = sequence_number;
    p.side = side;
    p.price = price;
    p.total_volume = total_volume;
    p.timestamp = timestamp;
    return p;
}