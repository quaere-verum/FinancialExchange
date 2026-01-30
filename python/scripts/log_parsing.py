from __future__ import annotations

from pathlib import Path
import os
import subprocess

import duckdb
import numpy as np
import polars as pl
import matplotlib.pyplot as plt


# -----------------------------
# Granularities per diagnostic
# -----------------------------
# You can tweak these later without changing the core logic.
BUCKETS = {
    # book state / spread shape
    "spread_dist": ["1ms", "10ms", "100ms"],
    # clustering / volatility-volume at multiple scales
    "activity_vs_vol": ["10ms", "100ms", "1s"],
    # micro impact should stay fine
    "impact": ["1ms", "10ms"],
    # persistence depends heavily on sampling; do a couple
    "flow_persistence": ["10ms", "100ms"],
    # return dynamics across micro + meso
    "mid_dynamics": ["1ms", "100ms", "1s"],
}

HORIZONS_BY_BUCKET = {
    # horizons are in "buckets" (i.e., units of bucket width)
    # keep small buckets with more lags; coarser buckets with fewer
    "1ms": [1, 2, 5, 10, 20, 50],
    "10ms": [1, 2, 5, 10, 20],
    "100ms": [1, 2, 5, 10],
    "1s": [1, 2, 5],
}

MAX_SPREAD_TICKS = 50
MAX_SHOW_TICKS = 20

ACF_LAGS_FLOW_SIGN = 50  # on bucket sign(flow) series


# -----------------------------
# Time helpers
# -----------------------------
def bucket_ns(bucket: str) -> int:
    bucket = bucket.strip().lower()
    if bucket.endswith("ns"):
        return int(bucket[:-2])
    if bucket.endswith("μs"):
        return int(bucket[:-2]) * 1_000
    if bucket.endswith("us"):
        return int(bucket[:-2]) * 1_000
    if bucket.endswith("ms"):
        return int(bucket[:-2]) * 1_000_000
    if bucket.endswith("s"):
        return int(bucket[:-1]) * 1_000_000_000
    raise ValueError(f"Unsupported bucket format: {bucket!r}")


# -----------------------------
# Stats helpers
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
    denom = float(np.dot(x, x))
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
# DuckDB aggregation
# -----------------------------
def bucketed_view(con: duckdb.DuckDBPyConnection, plu_glob: Path, trd_glob: Path, bucket_str: str) -> pl.DataFrame:
    bns = bucket_ns(bucket_str)

    query = f"""
    WITH
    -- Keep ts as UBIGINT (uint64) end-to-end
    q_range AS (
        SELECT
            min(timestamp) AS min_ts,
            max(timestamp) AS max_ts
        FROM read_parquet('{plu_glob}')
    ),

    -- Compute bucket-aligned base and end in UBIGINT
    bounds AS (
        SELECT
            (min_ts // CAST({bns} AS UBIGINT)) * CAST({bns} AS UBIGINT) AS base_ts,
            ((max_ts // CAST({bns} AS UBIGINT)) + 1) * CAST({bns} AS UBIGINT) AS end_ts
        FROM q_range
    ),

    -- Generate boundaries using BIGINT offsets (safe because end_ts-base_ts is small for your window)
    boundaries AS (
        SELECT
            base_ts + CAST(off AS UBIGINT) AS ts,
            NULL::BIGINT AS spread_ticks,
            NULL::BIGINT AS mid_px2_ticks,
            0 AS is_quote
        FROM bounds,
             generate_series(
                0::BIGINT,
                CAST(CAST(end_ts - base_ts AS HUGEINT) AS BIGINT),
                CAST({bns} AS BIGINT)
             ) AS t(off)
    ),

    quotes AS (
        SELECT
            timestamp AS ts,
            spread_ticks,
            mid_px2_ticks,
            1 AS is_quote
        FROM read_parquet('{plu_glob}')
    ),

    stream0 AS (
        SELECT * FROM quotes
        UNION ALL
        SELECT * FROM boundaries
    ),

    -- Group id increments whenever we see a real quote (spread_ticks non-null)
    stream1 AS (
        SELECT
            ts,
            (ts // CAST({bns} AS UBIGINT)) AS bucket,
            spread_ticks,
            mid_px2_ticks,
            is_quote,
            SUM(CASE WHEN spread_ticks IS NOT NULL THEN 1 ELSE 0 END)
                OVER (ORDER BY ts) AS grp
        FROM stream0
    ),

    -- Forward-fill via partition max (DuckDB-compatible)
    stream AS (
        SELECT
            ts,
            bucket,
            MAX(spread_ticks) OVER (PARTITION BY grp) AS spread_ff,
            MAX(mid_px2_ticks) OVER (PARTITION BY grp) AS mid_ff,
            is_quote
        FROM stream1
    ),

    -- Durations computed in signed 128-bit so subtraction is safe
    segments AS (
        SELECT
            bucket,
            ts,
            spread_ff,
            mid_ff,
            is_quote,
            CAST(LEAD(ts) OVER (ORDER BY ts) AS HUGEINT) - CAST(ts AS HUGEINT) AS duration
        FROM stream
    ),

    quote_state_agg AS (
        SELECT
            bucket,
            SUM(spread_ff * duration) / NULLIF(SUM(duration), 0) AS twa_spread,
            arg_max(mid_ff, ts) / 2.0 AS last_mid
        FROM segments
        WHERE duration > 0
          AND spread_ff IS NOT NULL
          AND mid_ff IS NOT NULL
        GROUP BY 1
    ),

    quote_event_agg AS (
        SELECT
            (timestamp // CAST({bns} AS UBIGINT)) AS bucket,
            COUNT(*) AS n_quote_events
        FROM read_parquet('{plu_glob}')
        GROUP BY 1
    ),

    trade_agg AS (
        SELECT
            (timestamp // CAST({bns} AS UBIGINT)) AS bucket,
            COUNT(*) AS n_trades,
            SUM(quantity) AS qty_sum,
            SUM(price * CAST(quantity AS BIGINT)) AS px_qty_sum,
            SUM(signed_volume) AS flow,
            arg_max(ewma_variance, timestamp) AS ewma_variance,
            arg_max(ewma_imbalance, timestamp) AS ewma_imbalance
        FROM read_parquet('{trd_glob}')
        GROUP BY 1
    )

    SELECT
        q.bucket,
        q.last_mid AS mid,
        q.twa_spread AS spread,
        COALESCE(qe.n_quote_events, 0) AS n_quote_events,
        COALESCE(t.n_trades, 0) AS n_trades,
        t.px_qty_sum / NULLIF(t.qty_sum, 0) AS vwap,
        COALESCE(t.flow, 0.0) AS flow,
        t.ewma_variance,
        t.ewma_imbalance
    FROM quote_state_agg q
    LEFT JOIN quote_event_agg qe ON q.bucket = qe.bucket
    LEFT JOIN trade_agg t ON q.bucket = t.bucket
    ORDER BY q.bucket
    """

    df = con.execute(query).pl()
    df = df.with_columns(
        [
            pl.col("ewma_variance").forward_fill(),
            pl.col("ewma_imbalance").forward_fill(),
        ]
    )
    return df

# -----------------------------
# Per-diagnostic computations
# -----------------------------
def run_spread_distribution(df: pl.DataFrame, bucket_str: str) -> None:
    spread = df["spread"].to_numpy().astype(np.float64)
    vol = df["ewma_variance"].to_numpy().astype(np.float64)
    imb = df["ewma_imbalance"].to_numpy().astype(np.float64)

    ok_spread = np.isfinite(spread) & (spread >= 0)
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
    print(f"SPREAD DISTRIBUTION (bucket={bucket_str})")
    print("==============================")
    print_summary("ALL BUCKETS", counts_all, over_all)
    print_summary("TOP DECILE VOL (bucket ewma_variance)", counts_vol, over_vol)
    print_summary("TOP DECILE |IMB| (bucket ewma_imbalance)", counts_imb, over_imb)

    # Optional plots
    # plot_counts(f"Spread histogram - ALL (bucket={bucket_str})", counts_all, over_all, MAX_SPREAD_TICKS, MAX_SHOW_TICKS)
    # plt.show()


def run_activity_vs_vol(df: pl.DataFrame, bucket_str: str) -> None:
    vol = df["ewma_variance"].to_numpy().astype(np.float64)
    n_trades = df["n_trades"].to_numpy().astype(np.float64)
    n_quotes_b = df["n_quote_events"].to_numpy().astype(np.float64)

    trade_presence = float(np.mean(n_trades > 0)) if n_trades.size else np.nan

    print("\n==============================")
    print(f"ACTIVITY vs VOL (bucket={bucket_str})")
    print("==============================")
    print(f"  P(n_trades>0) = {trade_presence: .3f}")
    print(f"  Corr(n_trades, ewma_variance) = {corr_pair(n_trades, vol): .6f}")
    print(f"  Corr(n_quote_events, ewma_variance) = {corr_pair(n_quotes_b, vol): .6f}")


def run_impact_and_effective_spread(df: pl.DataFrame, bucket_str: str) -> None:
    mid = df["mid"].to_numpy().astype(np.float64)
    flow = df["flow"].to_numpy().astype(np.float64)
    n_trades = df["n_trades"].to_numpy().astype(np.float64)
    vwap = df["vwap"].to_numpy().astype(np.float64)

    print("\n==============================")
    print(f"IMPACT / SPREAD METRICS (bucket={bucket_str})")
    print("==============================")

    # Effective spread proxy (bucket VWAP vs bucket mid), only buckets with trades
    has_tr = (n_trades > 0) & np.isfinite(vwap) & np.isfinite(mid)
    eff = 2.0 * np.abs(vwap[has_tr] - mid[has_tr])
    if eff.size:
        print("  Effective spread proxy (2*|VWAP-mid|) over trade buckets:")
        print(f"    n={eff.size:,}  mean={eff.mean():.6g}  p50={np.quantile(eff,0.5):.6g}  p95={np.quantile(eff,0.95):.6g}")
    else:
        print("  Effective spread proxy: no trade buckets with finite VWAP/mid.")

    # Response / impact at horizons
    flow_sign = np.sign(flow).astype(np.float64, copy=False)
    flow_sign[~np.isfinite(flow_sign)] = 0.0

    horizons = HORIZONS_BY_BUCKET.get(bucket_str, [1, 2, 5, 10])
    for h in horizons:
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


def run_flow_persistence(df: pl.DataFrame, bucket_str: str) -> None:
    flow = df["flow"].to_numpy().astype(np.float64)
    flow_sign = np.sign(flow).astype(np.float64, copy=False)
    flow_sign[~np.isfinite(flow_sign)] = 0.0

    # Two views:
    # (1) Clock-time with zeros (includes silence)
    # (2) Event-time on non-zero buckets only (persistence conditional on trading)
    print("\n==============================")
    print(f"ORDER-FLOW PERSISTENCE (bucket={bucket_str})")
    print("==============================")

    acf = autocorr(flow_sign, ACF_LAGS_FLOW_SIGN)
    for lag in [1, 2, 5, 10, 20, 50]:
        if lag <= ACF_LAGS_FLOW_SIGN:
            print(f"  ACF_clock(sign(flow))[{lag:2d}] = {acf[lag-1]: .6f}")

    nz = flow_sign[flow_sign != 0.0]
    acf_nz = autocorr(nz, min(ACF_LAGS_FLOW_SIGN, max(1, nz.size - 2))) if nz.size else np.full(ACF_LAGS_FLOW_SIGN, np.nan)
    for lag in [1, 2, 5, 10, 20, 50]:
        if lag <= (acf_nz.size if hasattr(acf_nz, "size") else 0):
            print(f"  ACF_event(sign(flow)|flow!=0)[{lag:2d}] = {acf_nz[lag-1]: .6f}")


def run_mid_dynamics(df: pl.DataFrame, bucket_str: str) -> None:
    mid = df["mid"].to_numpy().astype(np.float64)

    print("\n==============================")
    print(f"MID-PRICE DYNAMICS (bucket={bucket_str})")
    print("==============================")

    ok_mid = np.isfinite(mid)
    mid_v = mid[ok_mid]
    dmid = np.diff(mid_v)
    if dmid.size:
        print(f"  Δmid: mean={dmid.mean():.6g}  std={dmid.std(ddof=0):.6g}  p95(|Δ|)={np.quantile(np.abs(dmid),0.95):.6g}")
        acf_r = autocorr(dmid, 10)
        for lag in [1, 2, 5, 10]:
            print(f"  ACF(Δmid)[{lag:2d}] = {acf_r[lag-1]: .6f}")
    else:
        print("  Δmid: insufficient finite mid samples.")


# -----------------------------
# Main
# -----------------------------
def main() -> None:
    # Preprocess if needed
    if not os.path.isdir(Path("out", "order_book_stats")):
        print("PREPROCESSING STARTING")
        subprocess.run([Path("build", "apps", "parser", "Parser.exe"), "logs", "out", str(500_000)])
        print("PREPROCESSING DONE")

    plu_glob = Path("out", "order_book_stats", "*.parquet")
    trd_glob = Path("out", "trade", "*.parquet")

    con = duckdb.connect(database=":memory:")
    con.execute("SET temp_directory = 'duckdb_temp/'")
    con.execute("SET max_memory = '4GB'")

    print("\n==============================")
    print("BOOK HEALTH (NO BUCKETING)")
    print("==============================")

    health = con.execute(f"""
        SELECT
            count(*) as n_quotes,
            sum(CAST(book_valid AS INT)) as n_valid,
            sum(CAST(is_crossed AS INT)) as n_crossed
        FROM read_parquet('{plu_glob}')
    """).fetchone()

    n_trades_total = con.execute(f"""
        SELECT count(*) as n_trades
        FROM read_parquet('{trd_glob}')
    """).fetchone()[0]

    n_quotes, n_valid, n_cross = health
    print(f"Quotes: {n_quotes:,}")
    print(f"Trades: {n_trades_total:,}")
    print(f"  book_valid=True: {n_valid:,} ({(n_valid / n_quotes) if n_quotes else 0.0:6.2%})")
    print(f"  is_crossed=True: {n_cross:,} ({(n_cross / n_quotes) if n_quotes else 0.0:6.2%})")

    # Build the union of bucket widths we will need across diagnostics
    buckets_needed = sorted({b for bs in BUCKETS.values() for b in bs}, key=bucket_ns)

    # Cache bucketed frames to avoid repeated heavy scans
    cache: dict[str, pl.DataFrame] = {}

    def get_df(bucket_str: str) -> pl.DataFrame:
        if bucket_str not in cache:
            print(f"\n[DuckDB] Aggregating bucket={bucket_str} ...")
            cache[bucket_str] = bucketed_view(con, plu_glob, trd_glob, bucket_str)
        return cache[bucket_str]

    # 1) Spread distributions at book-state scales
    for b in BUCKETS["spread_dist"]:
        run_spread_distribution(get_df(b), b)

    # 2) Activity vs vol at meso scales
    for b in BUCKETS["activity_vs_vol"]:
        run_activity_vs_vol(get_df(b), b)

    # 3) Impact / effective spread at micro scales
    for b in BUCKETS["impact"]:
        run_impact_and_effective_spread(get_df(b), b)

    # 4) Order-flow persistence at a couple of scales
    for b in BUCKETS["flow_persistence"]:
        run_flow_persistence(get_df(b), b)

    # 5) Mid-price dynamics at micro + meso scales
    for b in BUCKETS["mid_dynamics"]:
        run_mid_dynamics(get_df(b), b)

    # Optional: if you want to force materialization in a controlled order
    _ = [get_df(b) for b in buckets_needed]


if __name__ == "__main__":
    main()
