# Avellaneda–Stoikov Market Making Engine (C++)

## Overview
This project implements a limit order book simulator and a market making engine in C++, based on the well-known Avellaneda–Stoikov (2008) framework for optimal market making.

It showcases:
- A C++ limit order book (LOB) with order adds, cancels, and trades.
- Avellaneda–Stoikov quoting logic: inventory-aware skewing of bid/ask prices.
- Risk controls: dynamic quote sizing, soft/hard inventory limits, and risk-off mode.
- A live terminal dashboard that displays BB/BA, mid, inventory, cash, PnL, and trade counts.
- CSV logging for PnL and inventory time-series, making it easy to plot results.

## Features
- Inventory-aware skewing: quotes tilt automatically when long/short to reduce inventory risk.
- Soft/hard limits: stops quoting the risky side as you approach limits.
- Risk-off mode: triggered by drawdowns or hard limits; optionally auto-liquidates back to flat.
- Dynamic sizing: quote size scales down as inventory grows, minimizing exposure.
- Synthetic flow (optional): add balanced buy/sell aggressor flow to stress-test the strategy.



This CSV can be used to plot PnL, inventory, and mid-price over time.

## How to Build & Run

### Requirements
- macOS/Linux with clang++ or g++ supporting C++20
- (Optional) Python + matplotlib for plotting results

### Build
```bash
mkdir -p build
clang++ -std=c++20 -O2 src/lob_single.cpp -o build/lob_demo


Key flags:
--tick : $ value per price tick
--delta : half-spread (ticks)
--qty : base quote size
--lambda : inventory risk-aversion parameter
--invsoft / --invhard : soft/hard inventory limits
--maxdd : max drawdown before risk-off
--buyrate / --sellrate : synthetic flow aggressor probabilities
Avellaneda–Stoikov (2008)
The model comes from Avellaneda, M. & Stoikov, S. (2008), High-frequency trading in a limit order book.
It derives optimal quoting rules by maximizing expected utility of terminal wealth.
Reservation price: shifts with inventory (implemented here).
Optimal spread: widens with volatility & risk aversion (a natural extension).
Extensions (future work)
Volatility-adaptive spreads (δ depends on σ²).
Time-to-horizon logic (tighten spreads near session end).
Multi-venue simulation with cross-exchange arbitrage.
Latency modeling (delayed fills, queue position).
