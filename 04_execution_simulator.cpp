#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

// Lesson 04: Deterministic bar-based futures execution.

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit, Stop };

struct ContractSpec {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    double tick_size{};
    double multiplier{};
};

struct Bar {
    std::int64_t ts{};
    double open{};
    double high{};
    double low{};
    double close{};
};

struct Order {
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    int quantity{};
    std::optional<double> limit_price;
    std::optional<double> stop_price;
};

struct ExecutionConfig {
    double slippage_ticks{0.0};
    double spread_ticks{0.0};
    double commission_per_contract_usd{0.0};
};

struct Fill {
    std::int64_t ts_fill{};
    Side side{Side::Buy};
    int quantity{};
    double fill_price{};
    double tick_size{};
    double multiplier{};
    double slippage_ticks{};
    double commission_usd{};
};

double adverse_adjustment(Side side, const ContractSpec& c, const ExecutionConfig& cfg) {
    double ticks = cfg.slippage_ticks + cfg.spread_ticks;
    double amount = ticks * c.tick_size;
    return side == Side::Buy ? amount : -amount;
}

std::optional<Fill> execute(
    const Order& order,
    const Bar& bar,
    const ContractSpec& c,
    const ExecutionConfig& cfg) {

    if (order.quantity <= 0) throw std::runtime_error("quantity must be positive");

    double reference_price = 0.0;
    bool marketable = false;

    if (order.type == OrderType::Market) {
        reference_price = bar.open;
        marketable = true;
    } else if (order.type == OrderType::Limit) {
        if (!order.limit_price) throw std::runtime_error("missing limit price");
        double L = *order.limit_price;

        if (order.side == Side::Buy) {
            if (bar.low > L) return std::nullopt;
            reference_price = bar.open <= L ? bar.open : L;
        } else {
            if (bar.high < L) return std::nullopt;
            reference_price = bar.open >= L ? bar.open : L;
        }
    } else if (order.type == OrderType::Stop) {
        if (!order.stop_price) throw std::runtime_error("missing stop price");
        double S = *order.stop_price;

        if (order.side == Side::Buy) {
            if (bar.high < S) return std::nullopt;
            reference_price = bar.open >= S ? bar.open : S;
        } else {
            if (bar.low > S) return std::nullopt;
            reference_price = bar.open <= S ? bar.open : S;
        }
        marketable = true;
    }

    double fill_price = reference_price;
    double applied_ticks = 0.0;
    if (marketable) {
        fill_price += adverse_adjustment(order.side, c, cfg);
        applied_ticks = cfg.slippage_ticks + cfg.spread_ticks;
    }

    double commission = cfg.commission_per_contract_usd * order.quantity;
    return Fill{bar.ts, order.side, order.quantity, fill_price,
                c.tick_size, c.multiplier, applied_ticks, commission};
}

int main() {
    ContractSpec nq{42004177, "NQU6", 0.25, 20.0};
    Bar bar{100, 30000.0, 30008.0, 29996.0, 30005.0};
    ExecutionConfig cfg{2.0, 1.0, 2.50};

    Order market_buy{Side::Buy, OrderType::Market, 1, std::nullopt, std::nullopt};
    auto market_fill = execute(market_buy, bar, nq, cfg);
    std::cout << "Market buy fill = " << market_fill->fill_price << '\n';

    Order buy_limit{Side::Buy, OrderType::Limit, 1, 29998.0, std::nullopt};
    auto limit_fill = execute(buy_limit, bar, nq, cfg);
    if (limit_fill) std::cout << "Buy limit fill = " << limit_fill->fill_price << '\n';

    Order buy_stop{Side::Buy, OrderType::Stop, 1, std::nullopt, 30006.0};
    auto stop_fill = execute(buy_stop, bar, nq, cfg);
    if (stop_fill) std::cout << "Buy stop fill = " << stop_fill->fill_price << '\n';

    return 0;
}
