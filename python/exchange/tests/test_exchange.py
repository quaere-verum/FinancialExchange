from exchange.agents.trader import Trader
from exchange.util import Side
import time
import subprocess
import os
import tqdm
import threading

def test_cancel_vs_match(trader_maker: Trader, trader_taker: Trader, price=100, qty=10, iters=1000):
    for idx in range(iters):
        barrier = threading.Barrier(2)

        trader_maker.insert_order(price, qty, Side.SELL)

        time.sleep(0.01)
        order_id = None
        for _ in range(100):  # try up to 0.5s
            open_orders = trader_maker.get_open_orders()
            if open_orders:
                order_id = next(iter(open_orders))
                break
            time.sleep(0.001)

        if order_id is None:
            raise RuntimeError("Order not confirmed in time")

        time.sleep(0.01)

        def cancel():
            barrier.wait()
            trader_maker.cancel_order(order_id)

        def match():
            barrier.wait()
            trader_taker.insert_order(price, qty, Side.BUY)

        t1 = threading.Thread(target=cancel)
        t2 = threading.Thread(target=match)

        t1.start()
        t2.start()
        t1.join()
        t2.join()

        time.sleep(0.05)

        # ---- ASSERTIONS ----
        maker_order = trader_maker.get_open_orders().get(order_id)

        # Either fully filled or fully canceled
        if maker_order is not None:
            assert maker_order.quantity_remaining == 0, f"Partial fill + cancel bug at {idx}"

        # Taker should never receive > qty
        total_filled = sum(
            o.quantity_cumulative for o in trader_taker._open_orders.values()
        )
        assert total_filled <= qty

        while True:
            maker_open_orders = trader_maker.get_open_orders()
            if len(maker_open_orders) > 0:
                for order_id in maker_open_orders:
                    trader_maker.cancel_order(order_id)
                    time.sleep(0.01)
            else:
                break
        while True:
            taker_open_orders = trader_taker.get_open_orders()
            if len(taker_open_orders) > 0:
                for order_id in trader_taker.get_open_orders():
                    trader_taker.cancel_order(order_id)
                    time.sleep(0.01)
            else:
                break
        print("=" * 5 + f" {idx} " + 5 * "=")
        time.sleep(0.05)
    print("Test cancel vs. match passed")


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
    trader2 = Trader("TRADER2", "127.0.0.1", port)
    trader1.set_verbose(True)
    trader2.set_verbose(True)

    test_cancel_vs_match(trader1, trader2, iters=10)

    # trader1.insert_order(900, 10, Side.SELL)
    # trader1.subscribe()
    
    # time.sleep(0.25)

    # trader2.insert_order(900, 5, Side.BUY)
    # trader2.insert_order(895, 5, Side.BUY)
    # trader2.subscribe()

    # time.sleep(0.25)

    # trader1.print_book()
    # trader2.print_book()
    
    # time.sleep(0.25)

    # for order_id in trader1._open_orders:
    #     trader1.cancel_order(order_id)

    # for order_id in trader2._open_orders:
    #     trader2.amend_order(order_id, 15)

    # time.sleep(0.25)

    # trader1.print_book()
    # trader2.print_book()

    # time.sleep(0.25)

    # proc.terminate()
    # proc.wait()

if __name__ == "__main__":
    main()