from exchange.util import get_codec, HEADER_STRUCT, ProtocolCodec, Side, MessageType
import time
from typing import Iterator
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

HEADER_SIZE = HEADER_STRUCT.size

def decode_binary_stream(
    f,
    codec: ProtocolCodec,
) -> Iterator[dict]:
    buffer = bytearray()

    while True:
        chunk = f.read(4096)
        if not chunk:
            break

        buffer.extend(chunk)

        while True:
            if len(buffer) < HEADER_SIZE:
                break

            msg_type, payload_size = HEADER_STRUCT.unpack_from(buffer, 0)

            total_size = HEADER_SIZE + payload_size
            if len(buffer) < total_size:
                break

            payload_bytes = bytes(
                buffer[HEADER_SIZE:total_size]
            )

            decoded = codec.decode_from_header(
                message_type=msg_type,
                payload_size=payload_size,
                payload_bytes=payload_bytes,
            )

            yield decoded

            del buffer[:total_size]

class OrderBook:
    def __init__(self):
        self.bids: dict[int, int] = dict()
        self.asks: dict[int, int] = dict()

    @property
    def best_bid(self) -> int | None:
        if len(self.bids) == 0:
            return None
        return max(self.bids)
    
    @property
    def best_ask(self) -> int | None:
        if len(self.asks) == 0:
            return None
        return min(self.asks)
    
    def update(self, payload: dict[str, int]) -> None:
        price = payload["price"]
        quantity = payload["total_volume"]
        side = payload["side"]

        if side == Side.SELL:
            if quantity > 0:
                self.asks[price] = quantity
            elif price in self.asks:
                del self.asks[price]

        elif side == Side.BUY:
            if quantity > 0:
                self.bids[price] = quantity
            elif price in self.bids:
                del self.bids[price]

        else:
            raise ValueError

def main():
    codec = get_codec()
    
    best_bid: list[int] = []
    best_ask: list[int] = []

    timestamps: list[int] = []

    book = OrderBook()


    with open("logs/20260119_181643.bin", "rb") as f:
        data = f.read()

    offset = 0
    while offset + HEADER_SIZE <= len(data):
        mtype, payload_size = HEADER_STRUCT.unpack_from(data, offset)
        total_size = HEADER_SIZE + payload_size

        if offset + total_size > len(data):
            break  # incomplete final message (should not happen)

        payload_bytes = data[offset + HEADER_SIZE : offset + total_size]

        payload = codec.decode_from_header(
            message_type=mtype,
            payload_size=payload_size,
            payload_bytes=payload_bytes,
        )["payload"]

        offset += total_size
        if mtype != MessageType.PRICE_LEVEL_UPDATE:
            continue

        book.update(payload)
        best_bid.append(book.best_bid)
        best_ask.append(book.best_ask)
        timestamps.append(payload["timestamp"])
        print(timestamps[-1])

    timestamps = (np.array(timestamps, dtype=float) - timestamps[0]) * 1e-9
    best_bid = np.array(best_bid, dtype=float)
    best_ask = np.array(best_ask, dtype=float)
    
    bins = np.floor(timestamps).astype(int)
    bincount = np.bincount(bins)
    plt.title(f"Average updates per second: {np.mean(bincount):.0f}")

    plt.plot(timestamps, best_bid, label="Best Bid")
    plt.plot(timestamps, best_ask, label="Best Ask")

    plt.grid()
    plt.legend()
    plt.xlabel("Time (s)")
    plt.ylabel("Price (ticks)")
    plt.show()
    

if __name__ == "__main__":
    main()