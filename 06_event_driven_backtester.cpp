#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Lesson 06: A small event-driven futures backtester.
// This is intentionally much smaller than the production engine.
// It demonstrates the architecture:
// MarketEvent -> execute pending Signal -> update history -> Strategy -> queue next Signal.

enum class Side { Buy, Sell };

struct ContractSpec {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    double tick_size{};
    double multiplier{};
};

struct MarketEvent {
    std::int64_t ts{};
    std::uint64_t seq{};
    std::uint32_t instrument_id{};
    double open{};
    double high{};
    double low{};
    double close{};
};

struct Signal {
    std::string root_symbol;
    int target_units{};
};

struct Fill {
    std::int64_t ts{};
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    Side side{Side::Buy};
    int quantity{};
    double price{};
    double multiplier{};
};

struct Position {
    int units{};
    double avg_entry{};
    double multiplier{};
    std::string raw_symbol;
};

class Ledger {
public:
    int units(std::uint32_t id) const {
        auto it = positions_.find(id);
        return it == positions_.end() ? 0 : it->second.units;
    }

    void apply(const Fill& f) {
        int signed_qty = f.side == Side::Buy ? f.quantity : -f.quantity;
        auto& p = positions_[f.instrument_id];

        if (p.units == 0) {
            p = {signed_qty, f.price, f.multiplier, f.raw_symbol};
            return;
        }

        bool same_direction = (p.units > 0 && signed_qty > 0) || (p.units < 0 && signed_qty < 0);
        if (same_direction) {
            int old_abs = std::abs(p.units);
            int add_abs = std::abs(signed_qty);
            p.avg_entry = (p.avg_entry * old_abs + f.price * add_abs) / (old_abs + add_abs);
            p.units += signed_qty;
            return;
        }

        int closing = std::min(std::abs(p.units), std::abs(signed_qty));
        double direction = p.units > 0 ? 1.0 : -1.0;
        realized_ += (f.price - p.avg_entry) * p.multiplier * direction * closing;

        int new_units = p.units + signed_qty;
        if (new_units == 0) {
            p = Position{};
        } else if ((p.units > 0 && new_units < 0) || (p.units < 0 && new_units > 0)) {
            p = {new_units, f.price, f.multiplier, f.raw_symbol};
        } else {
            p.units = new_units;
        }
    }

    double realized() const { return realized_; }

private:
    std::map<std::uint32_t, Position> positions_;
    double realized_{};
};

class MomentumStrategy {
public:
    Signal decide(const std::string& root, const std::vector<double>& history) const {
        if (history.size() < 2) return {root, 0};
        double ret = history.back() - history[history.size() - 2];
        if (ret > 0.0) return {root, +1};
        if (ret < 0.0) return {root, -1};
        return {root, 0};
    }
};

int main() {
    std::map<std::uint32_t, ContractSpec> contracts;
    contracts[42004058] = {42004058, "NQM6", "NQ", 0.25, 20.0};
    contracts[42004177] = {42004177, "NQU6", "NQ", 0.25, 20.0};

    // Note the roll from NQM6 to NQU6.
    std::vector<MarketEvent> events{
        {1, 0, 42004058, 30000, 30005, 29995, 30002},
        {2, 1, 42004058, 30002, 30008, 30000, 30006},
        {3, 2, 42004177, 30320, 30330, 30310, 30325},
        {4, 3, 42004177, 30325, 30328, 30315, 30318},
        {5, 4, 42004177, 30318, 30335, 30317, 30332},
    };

    MomentumStrategy strategy;
    Ledger ledger;
    std::vector<double> history;
    std::optional<Signal> pending;
    std::uint32_t held_instrument_id = 0;

    for (const auto& event : events) {
        const auto& active = contracts.at(event.instrument_id);

        // 1) Execute the signal generated on the previous bar.
        if (pending) {
            // If the active raw contract changed, close the old raw contract first.
            if (held_instrument_id != 0 && held_instrument_id != active.instrument_id) {
                int old_units = ledger.units(held_instrument_id);
                if (old_units != 0) {
                    const auto& old_contract = contracts.at(held_instrument_id);

                    // Simplified study example only:
                    // we close at the current event open to demonstrate non-relabelled raw contracts.
                    Fill close_old{
                        event.ts,
                        old_contract.instrument_id,
                        old_contract.raw_symbol,
                        old_units > 0 ? Side::Sell : Side::Buy,
                        std::abs(old_units),
                        event.open,
                        old_contract.multiplier
                    };
                    ledger.apply(close_old);
                    std::cout << "ROLL CLOSE " << old_contract.raw_symbol
                              << " @ " << close_old.price << '\n';
                }
            }

            int current = ledger.units(active.instrument_id);
            int delta = pending->target_units - current;

            if (delta != 0) {
                Fill fill{
                    event.ts,
                    active.instrument_id,
                    active.raw_symbol,
                    delta > 0 ? Side::Buy : Side::Sell,
                    std::abs(delta),
                    event.open,
                    active.multiplier
                };
                ledger.apply(fill);
                held_instrument_id = active.instrument_id;

                std::cout << "EXECUTE " << active.raw_symbol
                          << " target=" << pending->target_units
                          << " fill=" << fill.price << '\n';
            }
        }

        // 2) Update history with only information available at this bar.
        history.push_back(event.close);

        // 3) Strategy decides a root-level target for the NEXT bar.
        Signal next = strategy.decide(active.root_symbol, history);
        pending = next;

        std::cout << "DECIDE ts=" << event.ts
                  << " contract=" << active.raw_symbol
                  << " close=" << event.close
                  << " next_target=" << next.target_units << '\n';
    }

    std::cout << "Realized PnL = $" << ledger.realized() << '\n';
    return 0;
}
