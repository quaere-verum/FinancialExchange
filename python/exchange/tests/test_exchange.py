from exchange.agents.trader import Trader
from exchange.util import Side
import time
import subprocess
import os

def main():
    port = 16000
    exchange_path = os.path.join("build", "FinancialExchange.exe")
    n_exchange_threads = 3
    proc = subprocess.Popen(
        [exchange_path, f"{port}", f"{n_exchange_threads}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(0.25)

    trader1 = Trader("TRADER1", "127.0.0.1", port)
    trader1.insert_order(900, 10, Side.SELL)
    trader1.subscribe()
    
    time.sleep(0.25)

    trader2 = Trader("TRADER2", "127.0.0.1", port)
    trader2.insert_order(900, 5, Side.BUY)
    trader2.insert_order(895, 5, Side.BUY)
    trader2.subscribe()

    time.sleep(0.25)

    trader1.print_book()
    trader2.print_book()
    
    time.sleep(0.25)

    for order_id in trader1._open_orders:
        trader1.cancel_order(order_id)

    for order_id in trader2._open_orders:
        trader2.amend_order(order_id, 15)

    time.sleep(0.25)

    trader1.print_book()
    trader2.print_book()

    time.sleep(0.25)

    proc.terminate()
    proc.wait()

if __name__ == "__main__":
    main()