# Agentic Alpha C++ Quant Core Study

This repository is a hands-on C++ learning project built alongside my larger quantitative research system, **Agentic Alpha Discovery and Validation System for Multi-Asset Futures**.

The goal is to understand the core C++ architecture behind a futures trading engine by rebuilding the most important components in smaller, readable, and executable examples.

Instead of learning C++ only through generic examples, each file focuses on a real quantitative trading concept such as futures contracts, signals, orders, execution, positions, PnL, and event-driven backtesting.

---

## What This Repository Covers

The core trading flow is:

```text
Market Data
    ↓
Contract / Market State
    ↓
Strategy Context
    ↓
Signal
    ↓
Order
    ↓
Hard Risk
    ↓
Execution Simulator
    ↓
Fill
    ↓
Position Ledger
    ↓
Portfolio State
    ↓
PnL / Exposure / Margin / Drawdown
    ↓
Event-Driven Backtest Result
```
