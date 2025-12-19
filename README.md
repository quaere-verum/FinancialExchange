# Low-Latency Exchange Matching Engine (C++ / Boost.Asio)

This project is a simplified electronic exchange implemented in modern C++. It
supports order entry, cancellation, amendment, price-time priority matching,
and real-time market data distribution over TCP using an asynchronous,
event-driven architecture.

## Architecture Overview

The system is composed of four main layers:

1. **Networking Layer**
   - TCP connections using Boost.Asio
   - Asynchronous, non-blocking I/O
   - Per-connection strands to ensure thread safety

2. **Session / Exchange Layer**
   - Manages client sessions
   - Routes protocol messages
   - Translates domain events to network messages

3. **Matching Engine**
   - Central limit order book
   - Price-time priority
   - No knowledge of networking or connections

4. **Market Data Feed**
   - Snapshot-on-subscribe
   - Incremental updates (trades, level changes)

## Threading Model

- A single `boost::asio::io_context` is used
- One or more worker threads may call `io_context::run()`
- Each client connection owns a `boost::asio::strand`
- All socket I/O and connection state mutations occur on the strand

The matching engine itself is single-threaded and invoked from the I/O context,
ensuring deterministic behaviour without locks.

## Protocol Overview

The exchange communicates using a binary message protocol:

- Fixed-size headers
- Explicit message type and payload size
- Little-endian encoding

Supported message types include:
- Order insert
- Order cancel
- Order amend
- Trade confirmation
- Order book snapshot
- Error / rejection messages

## Order Book

- Central limit order book
- Separate bid and ask sides
- Fixed price levels
- FIFO queues per price level

Matching behavior:
- Incoming orders match against the opposite side
- Partial fills supported
- Price-time priority enforced
- Filled maker orders are removed immediately

## Market Data

- Clients may subscribe to the market data feed
- Upon subscription, a full order book snapshot is sent
- Subsequent updates include trades and level updates
- Periodic snapshots are planned but not yet implemented

## Limitations 

- Single-threaded matching engine
- No persistence across restarts
- No TLS or authentication enforcement
- No recovery / replay mechanism

These omissions are intentional to keep the system focused and easy to reason
about. The architecture allows these features to be added incrementally.
