from exchange.agents.trader import Trader
from exchange.util import Side
import time
import subprocess
import os
import tqdm
import threading

def test_cancel_vs_match(iters: int = 1000):
    port_base = 15000
    exchange_path = os.path.join("build", "FinancialExchange.exe")
    n_exchange_threads = 3

    price = 100
    qty = 10
    for idx in range(iters):
        print("=" * 5 + f" {idx} " + "=" * 5)
        port = port_base + idx
        proc = subprocess.Popen(
            [exchange_path, f"{port}", f"{n_exchange_threads}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.1)

        trader_maker = Trader("TRADER1", "127.0.0.1", port)
        trader_taker = Trader("TRADER2", "127.0.0.1", port)
        trader_maker.set_verbose(True)
        trader_taker.set_verbose(True)

        time.sleep(0.05)

        barrier = threading.Barrier(2)



        trader_maker.insert_order(price, qty, Side.SELL)

        time.sleep(0.01)
        open_orders = trader_maker.get_open_orders()
        order_id = next(iter(open_orders))

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

        time.sleep(0.01)

        # ---- ASSERTIONS ----
        maker_order = trader_maker.get_open_orders().get(order_id)

        # Either fully filled or fully canceled
        if maker_order is not None:
            assert maker_order.quantity_remaining == 0, f"Partial fill + cancel bug at {idx}"

        # Taker should never receive > qty
        total_filled = sum(
            o.quantity_cumulative for o in trader_taker._open_orders.values()
        )
        try:
            assert total_filled <= qty
        except AssertionError as e:
            proc.terminate()
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            raise e

        proc.terminate()
        proc.kill()


        time.sleep(0.01)
    print("Test cancel vs. match passed")


def test_double_match(iters: int = 1000):
    port_base = 15000
    exchange_path = os.path.join("build", "FinancialExchange.exe")
    n_exchange_threads = 3

    price = 100
    qty = 10

    for idx in range(iters):
        print("=" * 5 + f" {idx} " + "=" * 5)
        port = port_base + idx
        proc = subprocess.Popen(
            [exchange_path, f"{port}", f"{n_exchange_threads}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.1)

        trader_maker = Trader("TRADER1", "127.0.0.1", port)
        trader_a = Trader("TRADER2", "127.0.0.1", port)
        trader_b = Trader("TRADER3", "127.0.0.1", port)

        trader_maker.set_verbose(True)
        trader_a.set_verbose(True)
        trader_b.set_verbose(True)

        time.sleep(0.05)

        trader_maker.insert_order(price, qty, Side.SELL)
        
        time.sleep(0.05)

        barrier = threading.Barrier(2)

        def buy_a():
            barrier.wait()
            trader_a.insert_order(price, qty, Side.BUY)

        def buy_b():
            barrier.wait()
            trader_b.insert_order(price, qty, Side.BUY)

        t1 = threading.Thread(target=buy_a)
        t2 = threading.Thread(target=buy_b)


        t1.start()
        t2.start()
        t1.join()
        t2.join()

        time.sleep(0.01)

        maker_open_orders = trader_maker.get_open_orders()
        open_orders_a = trader_a.get_open_orders()
        open_orders_b = trader_b.get_open_orders()
        if (len(open_orders_a) == 0 or len(open_orders_b) == 0) and len(maker_open_orders) == 0:
            continue
        else:
            filled = 0
            for trader in (trader_a, trader_b):
                for o in trader.get_open_orders().values():
                    filled += o.quantity_cumulative
            try:
                assert filled == qty, "Overfill or lost fill detected"
            except AssertionError as e:
                proc.terminate()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                raise e
        proc.terminate()
        try:
            proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    print("Test double match passed.")

def test_amend_vs_match(iters: int = 1000):
    port_base = 15000
    exchange_path = os.path.join("build", "FinancialExchange.exe")
    n_exchange_threads = 3

    price = 100
    qty = 10
    new_qty = 5

    for idx in range(iters):
        print("=" * 5 + f" {idx} " + "=" * 5)
        port = port_base + idx
        proc = subprocess.Popen(
            [exchange_path, f"{port}", f"{n_exchange_threads}"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(0.1)

        trader_maker = Trader("TRADER1", "127.0.0.1", port)
        trader_taker = Trader("TRADER2", "127.0.0.1", port)
        trader_maker.set_verbose(True)
        trader_taker.set_verbose(True)

        time.sleep(0.02)

        trader_maker.insert_order(price, qty, Side.SELL)
        time.sleep(0.05)

        order_id = next(iter(trader_maker.get_open_orders()))

        barrier = threading.Barrier(2)

        def amend():
            barrier.wait()
            trader_maker.amend_order(order_id, new_qty)

        def match():
            barrier.wait()
            trader_taker.insert_order(price, qty, Side.BUY)

        threading.Thread(target=amend).start()
        threading.Thread(target=match).start()

        time.sleep(0.02)

        open_orders_maker = trader_maker.get_open_orders()
        open_orders_taker = trader_taker.get_open_orders()

        if len(open_orders_taker) == 0:
            try:
                assert len(open_orders_maker) == 0, "Amend vs. match bug."
            except AssertionError as e:
                proc.terminate()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                raise e
        else:
            taker_resting_order = next(iter(open_orders_taker.values()))
            try:
                assert taker_resting_order.quantity_remaining == qty - new_qty, "Amend vs. match bug."
            except AssertionError as e:
                proc.terminate()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                raise e
        proc.terminate()
        try:
            proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    print("Amend vs. match test passed.")


def main():
    # test_cancel_vs_match(iters=10)
    # time.sleep(1.0)
    # test_double_match(iters=10)
    # time.sleep(1.0)
    test_amend_vs_match(iters=10)


if __name__ == "__main__":
    main()