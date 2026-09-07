#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Lesson 08: Signed futures prices and crossing zero.
//
// This is a learning implementation of the price-domain semantics we want for
// a general futures engine. It is especially important for CL (WTI crude oil),
// because futures prices have historically traded through zero into negative values.
//
// The key rule is:
//   A normalized futures price is NOT required to be positive.
//   It must be finite, normalized, and within a plausible magnitude.
//
// This lesson demonstrates:
//   - positive, zero, and negative normalized prices
//   - signed OHLC validation
//   - tick-grid rounding for negative prices
//   - market / limit / stop execution at negative prices
//   - PnL across zero
//   - gross exposure direction remains based on position, not price sign
//
// Compile:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic 08_signed_futures_prices.cpp -o lesson08
// Run:
//   ./lesson08

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit, Stop };

struct ContractSpec {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    double tick_size{};
    double multiplier{};
};

struct MarketBar {
    std::int64_t ts_ns{};
    std::uint32_t instrument_id{};
    double open{};
    double high{};
    double low{};
    double close{};
    std::int64_t volume{};
};

struct Order {
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    int quantity{};
    std::optional<double> limit_price;
    std::optional<double> stop_price;
};

struct Fill {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    Side side{Side::Buy};
    int quantity{};
    double fill_price{};
    double tick_size{};
    double multiplier{};
};

constexpr double kMaxPlausibleNormalizedPrice = 1e9;

bool is_plausible_normalized_price(double price) {
    // Sign is intentionally NOT checked.
    // Zero and negative values are legal normalized futures prices.
    return std::isfinite(price) && std::abs(price) < kMaxPlausibleNormalizedPrice;
}

void validate_bar(const MarketBar& bar) {
    if (bar.ts_ns <= 0) throw std::runtime_error("invalid timestamp");
    if (bar.instrument_id == 0) throw std::runtime_error("invalid instrument id");
    if (bar.volume < 0) throw std::runtime_error("negative volume");

    for (double p : {bar.open, bar.high, bar.low, bar.close}) {
        if (!is_plausible_normalized_price(p)) {
            throw std::runtime_error("invalid normalized price");
        }
    }

    // OHLC ordering is purely mathematical and works with negative values.
    if (bar.low > bar.open || bar.low > bar.close ||
        bar.high < bar.open || bar.high < bar.close ||
        bar.low > bar.high) {
        throw std::runtime_error("invalid OHLC relationship");
    }
}

double round_to_tick(double price, double tick_size) {
    if (!(tick_size > 0.0) || !std::isfinite(tick_size)) {
        throw std::runtime_error("invalid tick size");
    }
    // std::round is symmetric around zero for half-away-from-zero behavior.
    // The same formula is used for positive and negative prices.
    return std::round(price / tick_size) * tick_size;
}

Fill make_fill(const ContractSpec& contract,
               Side side,
               int quantity,
               double raw_execution_price) {
    if (quantity <= 0) throw std::runtime_error("quantity must be positive");
    if (!is_plausible_normalized_price(raw_execution_price)) {
        throw std::runtime_error("execution price is not normalized");
    }

    return Fill{
        contract.instrument_id,
        contract.raw_symbol,
        side,
        quantity,
        round_to_tick(raw_execution_price, contract.tick_size),
        contract.tick_size,
        contract.multiplier
    };
}

std::optional<Fill> execute_one_bar(const ContractSpec& contract,
                                    const Order& order,
                                    const MarketBar& bar,
                                    double adverse_slippage_ticks = 0.0) {
    validate_bar(bar);
    const double slip = adverse_slippage_ticks * contract.tick_size;

    if (order.type == OrderType::Market) {
        const double px = order.side == Side::Buy ? bar.open + slip : bar.open - slip;
        return make_fill(contract, order.side, order.quantity, px);
    }

    if (order.type == OrderType::Limit) {
        if (!order.limit_price) throw std::runtime_error("limit price required");
        const double L = *order.limit_price;

        if (order.side == Side::Buy) {
            if (bar.low > L) return std::nullopt;
            // If the market opens below the buy limit, the trader receives the
            // better opening price rather than being forced to pay the limit.
            const double px = bar.open <= L ? bar.open : L;
            return make_fill(contract, order.side, order.quantity, px);
        }

        if (bar.high < L) return std::nullopt;
        const double px = bar.open >= L ? bar.open : L;
        return make_fill(contract, order.side, order.quantity, px);
    }

    if (order.type == OrderType::Stop) {
        if (!order.stop_price) throw std::runtime_error("stop price required");
        const double S = *order.stop_price;

        if (order.side == Side::Buy) {
            if (bar.high < S) return std::nullopt;
            const double trigger_px = bar.open >= S ? bar.open : S;
            return make_fill(contract, order.side, order.quantity, trigger_px + slip);
        }

        if (bar.low > S) return std::nullopt;
        const double trigger_px = bar.open <= S ? bar.open : S;
        return make_fill(contract, order.side, order.quantity, trigger_px - slip);
    }

    throw std::runtime_error("unknown order type");
}

double realized_pnl(const Fill& entry, const Fill& exit) {
    if (entry.instrument_id != exit.instrument_id) {
        throw std::runtime_error("fills are for different contracts");
    }
    if (entry.quantity != exit.quantity) {
        throw std::runtime_error("lesson assumes equal entry/exit quantity");
    }

    const double direction = entry.side == Side::Buy ? 1.0 : -1.0;
    return (exit.fill_price - entry.fill_price)
        * entry.multiplier
        * entry.quantity
        * direction;
}

void expect_rejected(const MarketBar& bar) {
    try {
        validate_bar(bar);
        throw std::runtime_error("expected validation to fail");
    } catch (const std::runtime_error&) {
        // Expected for this compact lesson.
    }
}

int main() {
    std::cout << "=== Lesson 08: Signed Futures Prices ===\n\n";

    ContractSpec cl{9001, "CLM20", "CL", 0.01, 1000.0};

    // A realistic-style sequence that crosses through zero.
    const std::vector<double> closes{5.0, 1.0, 0.0, -5.0, -20.0, -10.0};
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double c = closes[i];
        MarketBar b{
            static_cast<std::int64_t>(i + 1),
            cl.instrument_id,
            c,
            c + 1.0,
            c - 1.0,
            c,
            100
        };
        validate_bar(b);
        std::cout << "Accepted CL close: " << c << '\n';
    }

    std::cout << '\n';

    // Explicit signed OHLC example.
    MarketBar negative_bar{100, cl.instrument_id, -10.0, -5.0, -25.0, -20.0, 1000};
    validate_bar(negative_bar);
    std::cout << "Valid negative OHLC bar accepted.\n";

    // Zero is a valid numeric price in the generic domain.
    MarketBar zero_bar{101, cl.instrument_id, 0.0, 1.0, -1.0, 0.0, 1000};
    validate_bar(zero_bar);
    std::cout << "Zero-price bar accepted.\n\n";

    // Invalid numeric domains still fail.
    MarketBar nan_bar = zero_bar;
    nan_bar.open = std::numeric_limits<double>::quiet_NaN();
    expect_rejected(nan_bar);

    MarketBar inf_bar = zero_bar;
    inf_bar.close = std::numeric_limits<double>::infinity();
    expect_rejected(inf_bar);

    MarketBar fixed_point_like = zero_bar;
    fixed_point_like.open = 30'000'000'000'000.0;
    fixed_point_like.high = fixed_point_like.open + 1.0;
    fixed_point_like.low = fixed_point_like.open - 1.0;
    fixed_point_like.close = fixed_point_like.open;
    expect_rejected(fixed_point_like);

    std::cout << "NaN / infinity / huge unscaled value rejected.\n\n";

    // Negative tick rounding uses exactly the same formula as positive prices.
    const double raw = -20.007;
    const double rounded = round_to_tick(raw, cl.tick_size);
    std::cout << "Negative tick rounding: " << raw << " -> " << rounded << '\n';

    // Market order at a negative open.
    MarketBar exec_bar{200, cl.instrument_id, -20.0, -15.0, -25.0, -18.0, 5000};
    Order market_buy{Side::Buy, OrderType::Market, 1, std::nullopt, std::nullopt};
    Fill market_fill = *execute_one_bar(cl, market_buy, exec_bar, 1.0);
    std::cout << "Market BUY fill with 1 tick slippage: " << market_fill.fill_price << '\n';

    // Buy limit below the market.
    Order buy_limit{Side::Buy, OrderType::Limit, 1, -21.0, std::nullopt};
    auto limit_fill = execute_one_bar(cl, buy_limit, exec_bar);
    std::cout << "Buy LIMIT -21 fill: "
              << (limit_fill ? std::to_string(limit_fill->fill_price) : "NO FILL") << '\n';

    // Buy stop above the open, still in negative territory.
    Order buy_stop{Side::Buy, OrderType::Stop, 1, std::nullopt, -17.0};
    auto stop_fill = execute_one_bar(cl, buy_stop, exec_bar, 1.0);
    std::cout << "Buy STOP -17 fill: "
              << (stop_fill ? std::to_string(stop_fill->fill_price) : "NO FILL") << '\n';

    // PnL across zero. No absolute value belongs in PnL arithmetic.
    Fill entry = make_fill(cl, Side::Buy, 1, -20.0);
    Fill exit = make_fill(cl, Side::Sell, 1, -10.0);
    const double pnl1 = realized_pnl(entry, exit);
    std::cout << "\nLong CL: BUY -20, SELL -10 -> PnL = $" << pnl1 << '\n';
    assert(std::abs(pnl1 - 10000.0) < 1e-9);

    Fill entry2 = make_fill(cl, Side::Buy, 1, 5.0);
    Fill exit2 = make_fill(cl, Side::Sell, 1, -5.0);
    const double pnl2 = realized_pnl(entry2, exit2);
    std::cout << "Long CL: BUY +5, SELL -5  -> PnL = $" << pnl2 << '\n';
    assert(std::abs(pnl2 + 10000.0) < 1e-9);

    // Exposure is a magnitude. Position direction comes from units.
    const int units = +1;
    const double mark_price = -20.0;
    const double gross_exposure = std::abs(units * mark_price * cl.multiplier);
    const double signed_exposure = units > 0 ? gross_exposure : -gross_exposure;

    std::cout << "\nExposure at CL=-20 with +1 contract:\n";
    std::cout << "  gross exposure  = $" << gross_exposure << '\n';
    std::cout << "  signed exposure = $" << signed_exposure << " (still LONG)\n";

    std::cout << "\nAll signed-price checks passed.\n";
    return 0;
}
