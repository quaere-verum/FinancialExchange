from __future__ import annotations

from pathlib import Path
import numpy as np
import polars as pl
import matplotlib.pyplot as plt
import subprocess
import os


# -----------------------------
# User config
# -----------------------------
RUN = "20260127_171744"

# Bucket width: choose based on your sim intensity.
# 100ms is a good starting point; 1s if still heavy.
BUCKET = "1ms"

def bucket_ns(bucket: str) -> int:
    # supports "100ms", "1s", "250ms", etc.
    bucket = bucket.strip().lower()
    if bucket.endswith("ms"):
        return int(bucket[:-2]) * 1_000_000
    if bucket.endswith("s"):
        return int(bucket[:-1]) * 1_000_000_000
    raise ValueError(f"Unsupported BUCKET format: {bucket!r}")

BNS = bucket_ns(BUCKET)

# Horizons in buckets (not events)
HORIZONS = [1, 2, 5, 10, 20, 50]

MAX_SPREAD_TICKS = 50
MAX_SHOW_TICKS = 20  # for plots

ACF_LAGS_FLOW_SIGN = 50  # ACF on bucket flow sign


# -----------------------------
# Helpers
# -----------------------------
def hist_counts_spread(x: np.ndarray, max_tick: int) -> tuple[np.ndarray, int]:
    x = x.astype(np.int64, copy=False)
    overflow = int(np.sum(x > max_tick))
    x_clip = np.minimum(x, max_tick)
    counts = np.bincount(x_clip, minlength=max_tick + 1)
    return counts, overflow


def print_summary(name: str, counts: np.ndarray, overflow: int) -> None:
    total = int(counts.sum() + overflow)
    print(f"\n{name}: total={total:,}")
    for k in range(1, 11):
        c = int(counts[k]) if k < len(counts) else 0
        p = (c / total) if total else 0.0
        print(f"  S={k:2d}: {c:>10,}  ({p:6.2%})")
    tail3 = int(counts[3:].sum() + overflow)
    print(f"  P(S>=3): {(tail3 / total) if total else 0.0:6.2%}")


def autocorr(x: np.ndarray, max_lag: int) -> np.ndarray:
    x = x.astype(np.float64, copy=False)
    x = x[np.isfinite(x)]
    n = x.size
    if n < max_lag + 2:
        return np.full(max_lag, np.nan)
    x = x - x.mean()
    denom = np.dot(x, x)
    if denom <= 0:
        return np.full(max_lag, np.nan)
    acf = np.empty(max_lag, dtype=np.float64)
    for lag in range(1, max_lag + 1):
        acf[lag - 1] = float(np.dot(x[:-lag], x[lag:]) / denom)
    return acf


def corr_pair(x: np.ndarray, y: np.ndarray) -> float:
    ok = np.isfinite(x) & np.isfinite(y)
    if ok.sum() < 3:
        return np.nan
    xv = x[ok].astype(np.float64, copy=False)
    yv = y[ok].astype(np.float64, copy=False)
    sx = xv.std(ddof=0)
    sy = yv.std(ddof=0)
    if sx == 0 or sy == 0:
        return np.nan
    return float(np.corrcoef(xv, yv)[0, 1])


def plot_counts(title: str, counts: np.ndarray, overflow: int, max_spread: int, max_show: int) -> None:
    xs = np.arange(0, min(max_show, len(counts)))
    ys = counts[: xs.size]
    plt.figure()
    plt.bar(xs, ys)
    plt.yscale("log")
    plt.grid(True, which="both", axis="y")
    plt.xlabel("Spread (ticks)")
    plt.ylabel("Count (log scale)")
    plt.title(f"{title} (overflow>{max_spread}: {overflow:,})")


# -----------------------------
# Main
# -----------------------------
def main() -> None:
    if not os.path.isdir(Path("out", RUN, "order_book_stats")):
        print("PRPROCESSING STARTING")
        subprocess.run(
            [
                Path("build", "apps", "parser", "Parser.exe"),
                "logs",
                RUN,
                "out",
                str(2_000_000)
            ]
        )
        print("PREPROCESSING DONE")
    plu_glob = Path("out", RUN, "order_book_stats", "*.parquet")
    trd_glob = Path("out", RUN, "trade", "*.parquet")

    # ---------- Lazy scans ----------
    plu = (
        pl.scan_parquet(str(plu_glob))
          .select([
              "timestamp",
              "sequence_number",
              "best_bid_price",
              "best_ask_price",
              "best_bid_volume",
              "best_ask_volume",
              "book_valid",
              "is_crossed",
              "spread_ticks",
              "mid_px2_ticks",
          ])
    )

    trd = (
        pl.scan_parquet(str(trd_glob))
          .select([
              "timestamp",
              "sequence_number",
              "trade_id",
              "price",
              "quantity",
              "taker_side",
              "signed_volume",
              "ewma_signed_vol",
              "ewma_abs_volume",
              "ewma_imbalance",
              "ewma_variance",
          ])
    )

    # ---------- Deduplicate on timestamp using max sequence_number ----------
    def keep_max_seq_per_timestamp(lf: pl.LazyFrame) -> pl.LazyFrame:
        return lf.sort(["timestamp", "sequence_number"]).group_by("timestamp").tail(1)

    plu_u = keep_max_seq_per_timestamp(plu)
    trd_u = keep_max_seq_per_timestamp(trd)

    # ---------- Cast ns -> Datetime(ns) and bucket key ----------
    # This is crucial: group_by_dynamic requires a Datetime/Date column.
    plu_b = plu_u.with_columns((pl.col("timestamp") // pl.lit(BNS)).alias("bucket"))
    trd_b = trd_u.with_columns((pl.col("timestamp") // pl.lit(BNS)).alias("bucket"))

    # =============================
    # 1) Book health (streaming aggregate)
    # =============================
    health = (
        plu_b
        .select([
            pl.len().alias("n_quotes"),
            pl.col("book_valid").sum().alias("n_valid"),
            pl.col("is_crossed").sum().alias("n_crossed"),
        ])
        .collect(engine="streaming")
    )

    n_quotes = int(health["n_quotes"][0])
    n_valid = int(health["n_valid"][0])
    n_cross = int(health["n_crossed"][0])

    print("\n==============================")
    print("BOOK HEALTH")
    print("==============================")
    print(f"Quotes: {n_quotes:,}")
    print(f"  book_valid=True: {n_valid:,} ({(n_valid / n_quotes) if n_quotes else 0.0:6.2%})")
    print(f"  is_crossed=True: {n_cross:,} ({(n_cross / n_quotes) if n_quotes else 0.0:6.2%})")

    # =============================
    # 2) Build bucketed quote series (last observation per bucket)
    # =============================
    quotes_b = (
        plu_b
        .sort(["bucket", "timestamp", "sequence_number"])
        .group_by("bucket")
        .agg([
            pl.len().alias("n_quote_events"),
            pl.col("timestamp").last().alias("ts_last"),
            pl.col("mid_px2_ticks").last().alias("mid_px2"),
            pl.col("spread_ticks").last().alias("spread"),
            pl.col("best_bid_price").last().alias("bid"),
            pl.col("best_ask_price").last().alias("ask"),
            pl.col("best_bid_volume").last().alias("bid_vol"),
            pl.col("best_ask_volume").last().alias("ask_vol"),
            pl.col("book_valid").last().alias("book_valid"),
            pl.col("is_crossed").last().alias("is_crossed"),
        ])
        .sort("bucket")
    )

    # =============================
    # 3) Build bucketed trade series (flow + vwap + state)
    # =============================
    trades_b = (
        trd_b
        .sort(["bucket", "timestamp", "sequence_number"])
        .group_by("bucket")
        .agg([
            pl.len().alias("n_trades"),
            pl.col("quantity").sum().alias("qty_sum"),
            (pl.col("price") * pl.col("quantity").cast(pl.Int64)).sum().alias("px_qty_sum"),
            pl.col("signed_volume").sum().alias("signed_vol_sum"),
            pl.col("timestamp").last().alias("tr_ts_last"),
            pl.col("ewma_variance").last().alias("ewma_variance"),
            pl.col("ewma_imbalance").last().alias("ewma_imbalance"),
            pl.col("ewma_abs_volume").last().alias("ewma_abs_volume"),
            pl.col("ewma_signed_vol").last().alias("ewma_signed_vol"),
        ])
        .with_columns([
            (pl.col("px_qty_sum") / pl.when(pl.col("qty_sum") > 0).then(pl.col("qty_sum")).otherwise(None)).alias("vwap"),
        ])
        .sort("bucket")
    )

    # =============================
    # 4) Join buckets and forward-fill trade state (so each quote bucket has last-known state)
    # =============================
    buckets = (
        quotes_b
        .join(trades_b, on="bucket", how="left")
        .with_columns([
            pl.col("ewma_variance").forward_fill(),
            pl.col("ewma_imbalance").forward_fill(),
            pl.col("ewma_abs_volume").forward_fill(),
            pl.col("ewma_signed_vol").forward_fill(),
            pl.col("n_trades").fill_null(0),
            pl.col("qty_sum").fill_null(0),
            pl.col("signed_vol_sum").fill_null(0.0),
        ])
        .select([
            "bucket",
            "ts_last",
            "n_quote_events",
            "mid_px2",
            "spread",
            "bid", "ask", "bid_vol", "ask_vol",
            "book_valid", "is_crossed",
            "n_trades", "qty_sum", "vwap", "signed_vol_sum",
            "ewma_variance", "ewma_imbalance", "ewma_abs_volume", "ewma_signed_vol",
        ])
    )

    # IMPORTANT: collect only the bucketed result (much smaller than event logs)
    df = buckets.collect(engine="streaming")

    # Convert to numpy (bucket-level, manageable)
    spread = df["spread"].to_numpy().astype(np.float64, copy=False)
    mid = df["mid_px2"].to_numpy().astype(np.float64, copy=False) / 2.0
    vol = df["ewma_variance"].to_numpy().astype(np.float64, copy=False)
    imb = df["ewma_imbalance"].to_numpy().astype(np.float64, copy=False)
    n_trades = df["n_trades"].to_numpy().astype(np.float64, copy=False)
    n_quotes_b = df["n_quote_events"].to_numpy().astype(np.float64, copy=False)
    vwap = df["vwap"].to_numpy().astype(np.float64, copy=False)
    flow = df["signed_vol_sum"].to_numpy().astype(np.float64, copy=False)

    ok_spread = np.isfinite(spread) & (spread >= 0)
    spread_v = spread[ok_spread].astype(np.int64, copy=False)

    # =============================
    # 5) Spread distribution + conditional (bucketed decile thresholds)
    # =============================
    vol_ok = np.isfinite(vol) & ok_spread
    imb_ok = np.isfinite(imb) & ok_spread

    vol_thr = np.quantile(vol[vol_ok], 0.90) if vol_ok.any() else np.nan
    imb_thr = np.quantile(np.abs(imb[imb_ok]), 0.90) if imb_ok.any() else np.nan

    mask_all = ok_spread
    mask_vol = vol_ok & (vol >= vol_thr) if np.isfinite(vol_thr) else np.zeros_like(ok_spread, dtype=bool)
    mask_imb = imb_ok & (np.abs(imb) >= imb_thr) if np.isfinite(imb_thr) else np.zeros_like(ok_spread, dtype=bool)

    counts_all, over_all = hist_counts_spread(spread[mask_all].astype(np.int64, copy=False), MAX_SPREAD_TICKS)
    counts_vol, over_vol = hist_counts_spread(spread[mask_vol].astype(np.int64, copy=False), MAX_SPREAD_TICKS)
    counts_imb, over_imb = hist_counts_spread(spread[mask_imb].astype(np.int64, copy=False), MAX_SPREAD_TICKS)

    print("\n==============================")
    print(f"SPREAD DISTRIBUTION (BUCKET={BUCKET})")
    print("==============================")
    print_summary("ALL BUCKETS", counts_all, over_all)
    print_summary("TOP DECILE VOL (bucket ewma_variance)", counts_vol, over_vol)
    print_summary("TOP DECILE |IMB| (bucket ewma_imbalance)", counts_imb, over_imb)

    # =============================
    # 6) Activity clustering: trades/quotes per bucket vs vol
    # =============================
    print("\n==============================")
    print("ACTIVITY vs VOL")
    print("==============================")
    print(f"  Corr(n_trades, ewma_variance) = {corr_pair(n_trades, vol): .6f}")
    print(f"  Corr(n_quote_events, ewma_variance) = {corr_pair(n_quotes_b, vol): .6f}")

    # =============================
    # 7) Bucketed response / impact and realized spread proxy
    # =============================
    # Response: Δmid at horizon h (in mid units)
    # Impact proxy: sign(flow) * Δmid
    # Effective spread proxy: 2*|VWAP - mid| (only buckets with trades)
    print("\n==============================")
    print("IMPACT / SPREAD METRICS (BUCKETED)")
    print("==============================")

    # Effective spread proxy (bucket VWAP vs bucket mid)
    has_tr = (n_trades > 0) & np.isfinite(vwap) & np.isfinite(mid)
    eff = 2.0 * np.abs(vwap[has_tr] - mid[has_tr])
    if eff.size:
        print(f"  Effective spread proxy (2*|VWAP-mid|) over trade buckets:")
        print(f"    n={eff.size:,}  mean={eff.mean():.6g}  p50={np.quantile(eff,0.5):.6g}  p95={np.quantile(eff,0.95):.6g}")
    else:
        print("  Effective spread proxy: no trade buckets with finite VWAP/mid.")

    # Impact/response at horizons
    flow_sign = np.sign(flow).astype(np.float64, copy=False)
    flow_sign[~np.isfinite(flow_sign)] = 0.0

    for h in HORIZONS:
        if h <= 0 or h >= mid.size:
            continue
        mid0 = mid[:-h]
        midf = mid[h:]
        fs = flow_sign[:-h]

        ok = np.isfinite(mid0) & np.isfinite(midf) & (fs != 0.0)
        if ok.sum() < 100:
            print(f"\n  Horizon {h} buckets: insufficient samples ({int(ok.sum()):,}).")
            continue

        resp = (midf[ok] - mid0[ok])
        imp = fs[ok] * resp

        print(f"\n  Horizon {h} buckets:")
        print(f"    mean response (midf-mid0) = {resp.mean(): .6g}")
        print(f"    mean signed impact sign(flow)*Δmid = {imp.mean(): .6g}")
        print(f"    p05/p50/p95 signed impact = {np.quantile(imp,0.05):.6g} / {np.quantile(imp,0.5):.6g} / {np.quantile(imp,0.95):.6g}")

    # =============================
    # 8) Order-flow persistence proxy (bucket sign ACF)
    # =============================
    print("\n==============================")
    print("ORDER-FLOW PERSISTENCE (BUCKET SIGN)")
    print("==============================")
    acf = autocorr(flow_sign, ACF_LAGS_FLOW_SIGN)
    for lag in [1, 2, 5, 10, 20, 50]:
        if lag <= ACF_LAGS_FLOW_SIGN:
            print(f"  ACF(sign(flow))[{lag:2d}] = {acf[lag-1]: .6f}")

    # =============================
    # 9) Mid-price microstructure sanity (bucket returns)
    # =============================
    print("\n==============================")
    print("MID-PRICE DYNAMICS (BUCKET RETURNS)")
    print("==============================")
    ok_mid = np.isfinite(mid)
    mid_v = mid[ok_mid]
    dmid = np.diff(mid_v)
    if dmid.size:
        print(f"  Δmid: mean={dmid.mean():.6g}  std={dmid.std(ddof=0):.6g}  p95(|Δ|)={np.quantile(np.abs(dmid),0.95):.6g}")
        acf_r = autocorr(dmid, 10)
        for lag in [1, 2, 5, 10]:
            print(f"  ACF(Δmid)[{lag:2d}] = {acf_r[lag-1]: .6f}")

    # =============================
    # Optional plots
    # =============================
    # plot_counts(f"Spread histogram - ALL (bucket={BUCKET})", counts_all, over_all, MAX_SPREAD_TICKS, MAX_SHOW_TICKS)
    # plt.show()


if __name__ == "__main__":
    main()
