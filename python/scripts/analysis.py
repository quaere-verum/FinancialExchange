import polars as pl
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np

RUN = "20260127_171744"

def main():
    plu_glob = Path("out", RUN, "trade", "*.parquet")

    variance = pl.read_parquet(plu_glob).select(pl.col("ewma_variance")).to_numpy()

    hist_vals, hist_bins = np.histogram(np.sqrt(variance), bins=10, density=False)

    for k in range(hist_vals.size):
        print(f"{hist_bins[k]:.4f} <= vol <= {hist_bins[k+1]:.4f}: {hist_vals[k]} ({hist_vals[k] / hist_vals.sum() * 100:.2f}%)")


if __name__ == "__main__":
    main()