import polars as pl
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np

def main():
    run = "20260123_225128"
    plu_path = Path("out", run, "order_book_stats", "book_state", "*.parquet")
    print(plu_path)

    plu = pl.read_parquet(plu_path)

    data = plu.select([pl.col("timestamp"), pl.col("spread_ticks"), pl.col("mid_px2_ticks")]).to_pandas()
    del plu
    data["time"] = ((data["timestamp"] - data["timestamp"].iat[0]) * 1e-9).astype(np.float32)
    data["mid_price"] = data["mid_px2_ticks"].astype(np.float32) / 2.0
    data["spread_ticks"] = data["spread_ticks"].fillna(0).astype(np.int32)
    data = data.drop(columns=["mid_px2_ticks", "timestamp"])
    
    fig, ax = plt.subplots()
    ax.plot(data["time"], data["mid_price"])
    ax.grid()

    fig2, ax2 = plt.subplots()
    ax2.hist(data["spread_ticks"])
    ax2.grid()
    ax2.set_yscale("log")
    plt.show()

if __name__ == "__main__":
    main()
