import socket
import threading
import abc
from exchange.util import (
    get_codec, 
    MessageType, 
    HEADER_STRUCT, 
    Lifespan, 
    Side,
    decode_c_string
)
from dataclasses import dataclass

@dataclass
class Order:
    price: int
    quantity: int
    quantity_remaining: int
    quantity_cumulative: int
    side: Side

def _recv_exact(sock: socket.socket, n: int):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk: raise ConnectionError("Closed")
        data += chunk
    return data

class Trader(abc.ABC):
    def __init__(self, name: str, host: str, port: int):
        self.name = name
        self.sock: socket.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((host, port))
        self.next_request_id = 1
        self.running = True

        self.lock = threading.Lock()

        # ONLY this thread should ever call sock.recv
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.recv_thread.start()

        self.codec = get_codec()

        self._bids: dict[int, int] = dict()
        self._asks: dict[int, int] = dict()
        self._open_orders: dict[int, Order] = dict()
        self._partial_fill_buffer: list[dict] = []

        self._verbose: bool = False

    def get_open_orders(self) -> dict[int, Order]:
        with self.lock:
            return dict(self._open_orders)

    def set_verbose(self, val: bool):
        self._verbose = val

    def _send(self, message_type: MessageType, *args, **fields):
        self.sock.sendall(self.codec.encode(message_type, *args, **fields))

    def _recv_loop(self):
        while self.running:
            try:
                header = _recv_exact(self.sock, 3)
                msg_type, size = HEADER_STRUCT.unpack(header)
                payload = _recv_exact(self.sock, size)
                self._on_message(msg_type, size, payload)
            except Exception as e:
                if self.running:
                    print(f"[{self.name}] Connection error: {e}")
                break

    def _on_message(self, message_type: int, size: int, payload: bytes):
        fields = self.codec.decode_from_header(message_type, size, payload)["payload"]
        match message_type:
            case MessageType.ORDER_BOOK_SNAPSHOT:
                self._on_order_book_snapshot(fields)
            case MessageType.PRICE_LEVEL_UPDATE:
                self._on_price_level_update(fields)
            case MessageType.CONFIRM_ORDER_INSERTED:
                self._on_confirm_order_inserted(fields)
            case MessageType.PARTIAL_FILL_ORDER:
                self._on_partial_fill(fields)
            case MessageType.CONFIRM_ORDER_CANCELLED:
                self._on_confirm_order_cancelled(fields)
            case MessageType.CONFIRM_ORDER_AMENDED:
                self._on_confirm_order_amended(fields)
            case MessageType.ERROR_MSG:
                self._on_error_message(fields)

    def _on_error_message(self, fields: dict[str, str]):
        if not self._verbose:
            return
        msg = decode_c_string(fields["message"])
        print(f"Request ID {fields["client_request_id"]} failed with message: {msg}\n")

    def _on_order_book_snapshot(self, fields: dict):
        # len(fields) = 4 * ORDER_BOOK_DEPTH (bids + asks, volumes + prices) + 1 (sequence number)
        for idx in range((len(fields) - 1) // 4):
            ask_price = fields[f"ask_prices[{idx}]"]
            ask_volume = fields[f"ask_volumes[{idx}]"]
            bid_price = fields[f"bid_prices[{idx}]"]
            bid_volume = fields[f"bid_volumes[{idx}]"]
            if ask_volume > 0:
                self._asks[ask_price] = ask_volume
            if bid_volume > 0:
                self._bids[bid_price] = bid_volume
        if self._verbose:
            print(f"[{self.name}] Processed order book snapshot.")
        return

    def _on_price_level_update(self, fields: dict):
        side = self._bids if fields["side"] == Side.BUY else self._asks
        volume = fields["total_volume"]
        price = fields["price"]
        if volume == 0 and price in side:
            del side[price]
        elif volume > 0:
            side[price] = volume
        if self._verbose:
            print(f"[{self.name}] Processed price level  update.")
        return
        
    def _on_confirm_order_inserted(self, fields: dict):
        with self.lock:
            self._open_orders[fields["exchange_order_id"]] = Order(
                fields["price"],
                fields["total_quantity"],
                fields["leaves_quantity"],
                fields["total_quantity"] - fields["leaves_quantity"],
                Side.SELL if fields["side"] == Side.SELL else Side.BUY
            )
        if self._verbose:
            print(f"[{self.name}] Processed confirm order inserted.")
        return

    def _on_partial_fill(self, fields: dict):
        with self.lock:
            if fields["exchange_order_id"]  not in self._open_orders:
                # fill messages from insert order matching before it becomes a resting order
                if self._verbose:
                    print(f"[{self.name}] Partial fill not in open orders.")
                return
            
            leaves_quantity = fields["leaves_quantity"]
            if leaves_quantity == 0:
                del self._open_orders[fields["exchange_order_id"]]
                return
            order = self._open_orders[fields["exchange_order_id"]]
            order.quantity_remaining = leaves_quantity
            order.quantity_cumulative = fields["cumulative_quantity"]
        if self._verbose:
            print(f"[{self.name}] Processed partial fill.")
        return

    def _on_confirm_order_cancelled(self, fields: dict):
        with self.lock:
            try:
                del self._open_orders[fields["exchange_order_id"]]
                if self._verbose:
                    print(f"[{self.name}] Processed order cancelled.")
            except KeyError:
                if self._verbose:
                    print(f"[{self.name}] Cancelled order not in open orders.")
        return

    def _on_confirm_order_amended(self, fields: dict):
        with self.lock:
            id = fields["exchange_order_id"]
            self._open_orders[id].quantity = fields["new_total_quantity"]
            self._open_orders[id].quantity_remaining = fields["leaves_quantity"]
        if self._verbose:
            print(f"[{self.name}] Processed confirm order amended.")
        return

    def insert_order(self, price: int, quantity: int, side: Side):
        self._send(MessageType.INSERT_ORDER, self.next_request_id, side, price, quantity, Lifespan.GOOD_FOR_DAY)
        if self._verbose:
            print(f"[{self.name}] Sent order insertion request.")
        self.next_request_id += 1

    def cancel_order(self, order_id: int):
        self._send(MessageType.CANCEL_ORDER, self.next_request_id, order_id)
        if self._verbose:
            print(f"[{self.name}] Sent order cancellation request.")
        self.next_request_id += 1

    def amend_order(self, order_id: int, new_volume: int):
        self._send(MessageType.AMEND_ORDER, self.next_request_id, order_id, new_volume)
        if self._verbose:
            print(f"[{self.name}] Sent order amendment request.")
        self.next_request_id += 1

    def subscribe(self):
        self._send(MessageType.SUBSCRIBE, self.next_request_id)
        self.next_request_id += 1

    def disconnect(self):
        self._send(MessageType.DISCONNECT, self.next_request_id)
        self.next_request_id += 1

    def print_book(self):
        print(self.name)
        print("===== BIDS =====")
        for price, vol in self._bids.items():
            print(f"{vol} @ {price}")
        print("===== ASKS =====")
        for price, vol in self._asks.items():
            print(f"{vol} @ {price}")
