from __future__ import annotations

import os
from pathlib import Path
import numpy as np
import matplotlib.pyplot as plt

from exchange.util import get_codec, MessageType, ProtocolCodec, PayloadSchema, Side, MESSAGE_TO_PAYLOAD
import heapq


# -----------------------------
# User config
# -----------------------------
LOG_DIR = Path("logs")
RUN_PREFIX = "20260123_185630"  # timestamp prefix used in filenames
MAX_SPREAD_TICKS = 50

TAU_VOL_SHORT = 1.0
TAU_FLOW = 2.0

def file_for(run_prefix: str, kind: str) -> Path:
    return LOG_DIR / f"{run_prefix}_{kind}.bin"


def schema_for_message(codec: ProtocolCodec, mtype: MessageType):
    message_name = codec.message_types[int(mtype)]
    payload_name = MESSAGE_TO_PAYLOAD[message_name]
    return codec.payload_schemas[payload_name]


def memmap_payload_file(path: Path, dtype: np.dtype) -> np.memmap:
    """
    Memory map a payload-only file as a structured numpy array.
    Truncates to whole-record length if file size isn't a multiple of record size.
    """
    rec_size = dtype.itemsize
    size = path.stat().st_size
    n = size // rec_size
    if n == 0:
        return np.memmap(path, mode="r", dtype=dtype, shape=(0,))
    return np.memmap(path, mode="r", dtype=dtype, shape=(n,))


def struct_to_numpy_dtype(schema: PayloadSchema) -> np.dtype:
    """
    Convert your parsed schema to a numpy dtype.
    Assumes schema provides:
      - schema.struct.format (python struct format string)
      - schema.field_names (list[str])
    """
    import re

    fmt = schema.struct.format
    field_names = list(schema.field_names)

    endian = "<"
    if fmt and fmt[0] in "<>!=":
        endian = fmt[0]
        fmt_body = fmt[1:]
    else:
        fmt_body = fmt

    if endian == "<":
        np_end = "<"
    elif endian in (">", "!"):
        np_end = ">"
    else:
        np_end = "=" 

    code_map = {
        "B": "u1", "b": "i1",
        "H": "u2", "h": "i2",
        "I": "u4", "i": "i4",
        "Q": "u8", "q": "i8",
        "f": "f4", "d": "f8",
    }

    tokens = re.findall(r"(\d*)([xcbBhHiIqQfdsp])", fmt_body)

    fields = []
    name_i = 0

    for count_str, code in tokens:
        count = int(count_str) if count_str else 1

        if code == "x":
            fields.append((f"_pad_{len(fields)}", f"V{count}"))
            continue

        if code == "s":
            fields.append((field_names[name_i], f"{np_end}S{count}"))
            name_i += 1
            continue

        if code not in code_map:
            raise ValueError(f"Unsupported struct code {code} in {fmt}")

        base = np.dtype(np_end + code_map[code])

        if count == 1:
            fields.append((field_names[name_i], base))
        else:
            fields.append((field_names[name_i], base, (count,)))
        name_i += 1

    if name_i != len(field_names):
        raise ValueError(f"Field mismatch: used {name_i}, schema has {len(field_names)}")

    return np.dtype(fields, align=False)

def ewma_alpha(dt: np.ndarray, tau: float) -> np.ndarray:
    dt = np.maximum(dt, 1e-12)
    return 1.0 - np.exp(-dt / tau)

class BestBook:
    """
    Maintains best bid/ask from level updates efficiently (lazy heaps).
    Stores volumes per price level; best retrieval uses heaps with lazy deletion.
    """
    def __init__(self):
        self.bid_vol = {}  # price -> vol
        self.ask_vol = {}  # price -> vol
        self.bid_heap = []  # max-heap via negative price
        self.ask_heap = []  # min-heap

    def update_level(self, price: int, side: int, total_volume: int, SideEnum) -> None:
        if side == int(SideEnum.BUY):
            if total_volume > 0:
                self.bid_vol[price] = total_volume
                heapq.heappush(self.bid_heap, -price)
            else:
                self.bid_vol.pop(price, None)
        elif side == int(SideEnum.SELL):
            if total_volume > 0:
                self.ask_vol[price] = total_volume
                heapq.heappush(self.ask_heap, price)
            else:
                self.ask_vol.pop(price, None)
        else:
            return

    def best_bid(self) -> int | None:
        while self.bid_heap:
            p = -self.bid_heap[0]
            if p in self.bid_vol and self.bid_vol[p] > 0:
                return p
            heapq.heappop(self.bid_heap)
        return None

    def best_ask(self) -> int | None:
        while self.ask_heap:
            p = self.ask_heap[0]
            if p in self.ask_vol and self.ask_vol[p] > 0:
                return p
            heapq.heappop(self.ask_heap)
        return None


def main():
    codec = get_codec()
    plu_schema = schema_for_message(codec, MessageType.PRICE_LEVEL_UPDATE)
    trade_schema = schema_for_message(codec, MessageType.TRADE_EVENT)

    plu_dt = struct_to_numpy_dtype(plu_schema)
    trade_dt = struct_to_numpy_dtype(trade_schema)

    plu_path = file_for(RUN_PREFIX, "price_level_update")
    trade_path = file_for(RUN_PREFIX, "trade")

    if not plu_path.exists():
        raise FileNotFoundError(plu_path)

    plu = memmap_payload_file(plu_path, plu_dt)
    trade = memmap_payload_file(trade_path, trade_dt) if trade_path.exists() else None

    book = BestBook()

    n = len(plu)
    spreads = np.empty(n, dtype=np.int16)
    spreads.fill(-1)
    
    vol_ewma = np.zeros(n, dtype=np.float64)
    abs_imb_at_plu = np.zeros(n, dtype=np.float64)

    if trade is not None and len(trade) > 0:
        for k in ["timestamp", "quantity", "taker_side"]:
            if k not in trade.dtype.names:
                raise RuntimeError(f"Trade payload missing field '{k}'. Available: {trade.dtype.names}")

        trade_ts = trade["timestamp"].astype(np.uint64, copy=False)
        trade_qty = trade["quantity"].astype(np.float64, copy=False)
        trade_side = trade["taker_side"].astype(np.int32, copy=False)
        ti = 0

        abs_vol = 0.0
        signed_vol = 0.0
        last_trade_ts = None
    else:
        trade_ts = trade_qty = trade_side = None
        ti = 0
        abs_vol = signed_vol = 0.0
        last_trade_ts = None

    last_mid = None
    last_mid_ts = None
    v = 0.0

    plu_ts = plu["timestamp"].astype(np.uint64, copy=False)
    plu_price = plu["price"].astype(np.uint64, copy=False)
    plu_qty = plu["total_volume"].astype(np.int64, copy=False)
    plu_side = plu["side"].astype(np.int32, copy=False)

    for i in range(n):
        ts = int(plu_ts[i])
        price = int(plu_price[i])
        qty = int(plu_qty[i])
        side = int(plu_side[i])

        book.update_level(price, side, qty, Side)

        bb = book.best_bid()
        ba = book.best_ask()
        if bb is None or ba is None or ba < bb:
            spreads[i] = -1
        else:
            spreads[i] = int(ba - bb)

        # Mid/vol EWMA
        if bb is not None and ba is not None and ba >= bb:
            mid = 0.5 * (bb + ba)
            if last_mid is None:
                last_mid = mid
                last_mid_ts = ts
                v = 0.0
            else:
                dt = max(1e-12, (ts - last_mid_ts) * 1e-9)
                r = np.log(mid / last_mid) if (mid > 0 and last_mid > 0) else 0.0
                a = 1.0 - np.exp(-dt / TAU_VOL_SHORT)
                v = (1.0 - a) * v + a * (r * r)
                last_mid = mid
                last_mid_ts = ts
        vol_ewma[i] = v

        # Flow EWMA sampled up to this PLU timestamp
        if trade_ts is not None:
            while ti < len(trade_ts) and int(trade_ts[ti]) <= ts:
                tts = int(trade_ts[ti])
                q = float(trade_qty[ti])
                s = int(trade_side[ti])

                if last_trade_ts is None:
                    abs_vol = q
                    signed_vol = q if s == int(Side.BUY) else -q
                    last_trade_ts = tts
                else:
                    dt = max(1e-12, (tts - last_trade_ts) * 1e-9)
                    a = 1.0 - np.exp(-dt / TAU_FLOW)
                    abs_vol = (1.0 - a) * abs_vol + a * q
                    signed = q if s == int(Side.BUY) else -q
                    signed_vol = (1.0 - a) * signed_vol + a * signed
                    last_trade_ts = tts

                ti += 1

            imb = signed_vol / (abs_vol + 1e-12)
            abs_imb_at_plu[i] = abs(np.clip(imb, -1.0, 1.0))
        else:
            abs_imb_at_plu[i] = 0.0

    # Filter valid spreads
    valid = spreads >= 0
    spreads_v = spreads[valid]
    vol_v = vol_ewma[valid]
    imb_v = abs_imb_at_plu[valid]

    # Top decile thresholds
    vol_thr = np.quantile(vol_v, 0.90) if len(vol_v) else 0.0
    imb_thr = np.quantile(imb_v, 0.90) if len(imb_v) else 0.0

    mask_vol = vol_v >= vol_thr
    mask_imb = imb_v >= imb_thr

    def hist_counts(x: np.ndarray, max_tick: int):
        overflow = int(np.sum(x > max_tick))
        x_clip = np.minimum(x, max_tick)
        counts = np.bincount(x_clip, minlength=max_tick + 1)
        return counts, overflow

    counts_all, over_all = hist_counts(spreads_v, MAX_SPREAD_TICKS)
    counts_vol, over_vol = hist_counts(spreads_v[mask_vol], MAX_SPREAD_TICKS)
    counts_imb, over_imb = hist_counts(spreads_v[mask_imb], MAX_SPREAD_TICKS)

    def print_summary(name: str, counts: np.ndarray, overflow: int):
        total = int(counts.sum() + overflow)
        print(f"\n{name}: total={total:,}, overflow(>{MAX_SPREAD_TICKS})={overflow:,}")
        for k in range(1, 11):
            c = int(counts[k]) if k < len(counts) else 0
            p = c / total if total else 0.0
            print(f"  S={k:2d}: {c:>10,}  ({p:6.2%})")
        tail3 = int(counts[3:].sum() + overflow)
        print(f"  P(S>=3): {tail3/total:6.2%}")

    print_summary("ALL", counts_all, over_all)
    print_summary("TOP DECILE VOL (EWMA r^2)", counts_vol, over_vol)
    print_summary("TOP DECILE |IMB| (EWMA signed/abs)", counts_imb, over_imb)

    # Plot histograms (first 20 ticks)
    def plot_counts(title: str, counts: np.ndarray, overflow: int, max_show: int = 20):
        xs = np.arange(0, min(max_show, len(counts)))
        ys = counts[:len(xs)]
        plt.figure()
        plt.bar(xs, ys)
        plt.yscale("log")
        plt.grid(True, which="both", axis="y")
        plt.xlabel("Spread (ticks)")
        plt.ylabel("Count (log scale)")
        plt.title(f"{title} (overflow>{MAX_SPREAD_TICKS}: {overflow:,})")

    plot_counts("Spread histogram - ALL", counts_all, over_all)
    plot_counts("Spread histogram - TOP DECILE VOL", counts_vol, over_vol)
    plot_counts("Spread histogram - TOP DECILE |IMB|", counts_imb, over_imb)

    plt.show()


if __name__ == "__main__":
    main()
