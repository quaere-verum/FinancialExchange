from exchange.util import get_codec, HEADER_STRUCT, ProtocolCodec, Side, MessageType
import mmap
import numpy as np
import matplotlib.pyplot as plt

HEADER_SIZE = HEADER_STRUCT.size

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

    best_bid: list[float] = []
    best_ask: list[float] = []
    timestamps: list[int] = []

    book = OrderBook()

    path = "logs/20260123_155141.bin"
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            n = mm.size()
            offset = 0

            # Optional: progress every N updates (avoid per-message printing)
            seen = 0
            progress_every = 1_000_000

            while offset + HEADER_SIZE <= n:
                mtype, payload_size = HEADER_STRUCT.unpack_from(mm, offset)
                total_size = HEADER_SIZE + payload_size

                if offset + total_size > n:
                    break  # incomplete final message

                if mtype != MessageType.PRICE_LEVEL_UPDATE:
                    offset += total_size
                    continue

                payload_bytes = mm[offset + HEADER_SIZE : offset + total_size]

                payload = codec.decode_from_header(
                    message_type=mtype,
                    payload_size=payload_size,
                    payload_bytes=payload_bytes,
                )["payload"]

                offset += total_size

                book.update(payload)
                best_bid.append(book.best_bid if book.best_bid is not None else np.nan)
                best_ask.append(book.best_ask if book.best_ask is not None else np.nan)
                timestamps.append(payload["timestamp"])

                seen += 1
                if seen % progress_every == 0:
                    print(f"Parsed {seen:,} PRICE_LEVEL_UPDATE messages")

        finally:
            mm.close()

    if not timestamps:
        raise RuntimeError("No PRICE_LEVEL_UPDATE messages found.")

    timestamps_arr = (np.array(timestamps, dtype=np.float64) - float(timestamps[0])) * 1e-9
    best_bid_arr = np.array(best_bid, dtype=np.float64)
    best_ask_arr = np.array(best_ask, dtype=np.float64)

    bins = np.floor(timestamps_arr).astype(np.int64)
    bincount = np.bincount(bins)

    updates_per_sec = np.mean(bincount)
    del bins
    del bincount


    fig, ax = plt.subplots(nrows=2)
    fig.suptitle(f"Average updates per second: {updates_per_sec:.0f}")
    
    ax0: plt.Axes = ax[0]
    ax1: plt.Axes = ax[1]

    ax0.plot(timestamps_arr, best_bid_arr, label="Best Bid")
    ax0.plot(timestamps_arr, best_ask_arr, label="Best Ask")

    ax0.grid()
    ax0.legend(loc="upper left")
    ax0.set_xlabel("Time (s)")
    ax0.set_ylabel("Price (ticks)")

    ax1.hist(best_ask_arr - best_bid_arr, label="Spread")
    ax1.grid()
    ax1.legend(loc="upper left")
    ax1.set_xlabel("Spread (ticks)")
    ax1.set_yscale("log")
    ax1.set_ylabel("Density")

    plt.show()

if __name__ == "__main__":
    main()
