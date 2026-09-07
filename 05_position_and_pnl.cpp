#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

// Lesson 05: Position accounting and futures PnL.

enum class Side { Buy, Sell };

struct Fill {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    Side side{Side::Buy};
    int quantity{};
    double fill_price{};
    double multiplier{};
    double commission_usd{};
};

struct Position {
    int units{};
    double avg_entry_price{};
    double multiplier{};
    std::string raw_symbol;
};

class PositionLedger {
public:
    void apply(const Fill& fill) {
        int signed_qty = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
        auto& p = positions_[fill.instrument_id];

        if (p.units == 0) {
            p.units = signed_qty;
            p.avg_entry_price = fill.fill_price;
            p.multiplier = fill.multiplier;
            p.raw_symbol = fill.raw_symbol;
            costs_ += fill.commission_usd;
            return;
        }

        bool same_direction = (p.units > 0 && signed_qty > 0) || (p.units < 0 && signed_qty < 0);
        if (same_direction) {
            int old_abs = std::abs(p.units);
            int add_abs = std::abs(signed_qty);
            p.avg_entry_price =
                (p.avg_entry_price * old_abs + fill.fill_price * add_abs) /
                static_cast<double>(old_abs + add_abs);
            p.units += signed_qty;
            costs_ += fill.commission_usd;
            return;
        }

        int closing_units = std::min(std::abs(p.units), std::abs(signed_qty));
        double direction = p.units > 0 ? 1.0 : -1.0;
        double pnl_per_unit = (fill.fill_price - p.avg_entry_price) * p.multiplier * direction;
        realized_pnl_ += pnl_per_unit * closing_units;

        int new_units = p.units + signed_qty;

        if (new_units == 0) {
            p = Position{};
        } else if ((p.units > 0 && new_units < 0) || (p.units < 0 && new_units > 0)) {
            // Flip through zero: leftover units form a new position at the new fill price.
            p.units = new_units;
            p.avg_entry_price = fill.fill_price;
            p.multiplier = fill.multiplier;
            p.raw_symbol = fill.raw_symbol;
        } else {
            // Partial reduction: average entry of remaining old position does not change.
            p.units = new_units;
        }

        costs_ += fill.commission_usd;
    }

    double realized_pnl() const { return realized_pnl_; }
    double costs() const { return costs_; }
    double net_realized_pnl() const { return realized_pnl_ - costs_; }

    double unrealized_pnl(std::uint32_t instrument_id, double mark_price) const {
        auto it = positions_.find(instrument_id);
        if (it == positions_.end() || it->second.units == 0) return 0.0;

        const auto& p = it->second;
        double direction = p.units > 0 ? 1.0 : -1.0;
        return (mark_price - p.avg_entry_price)
            * p.multiplier
            * std::abs(p.units)
            * direction;
    }

private:
    std::map<std::uint32_t, Position> positions_;
    double realized_pnl_{};
    double costs_{};
};

int main() {
    PositionLedger ledger;

    ledger.apply({42004177, "NQU6", Side::Buy, 1, 30000.0, 20.0, 2.50});
    std::cout << "Unrealized at 30010 = $"
              << ledger.unrealized_pnl(42004177, 30010.0) << '\n';

    ledger.apply({42004177, "NQU6", Side::Sell, 1, 30010.0, 20.0, 2.50});

    std::cout << "Gross realized PnL = $" << ledger.realized_pnl() << '\n';
    std::cout << "Costs = $" << ledger.costs() << '\n';
    std::cout << "Net realized PnL = $" << ledger.net_realized_pnl() << '\n';

    return 0;
}
