#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

// Lesson 03: Strategy intent -> contract resolution -> order delta -> risk decision.

enum class Side { Buy, Sell };
enum class RiskVerdict { Approve, Resize, Reject };

struct ContractSpec {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    double tick_size{};
    double multiplier{};
    std::int64_t activation_ns{};
    std::int64_t expiration_ns{};

    bool is_live_at(std::int64_t ts) const {
        return ts >= activation_ns && ts <= expiration_ns;
    }
};

class ContractRegistry {
public:
    void add(const ContractSpec& c) { contracts_[c.instrument_id] = c; }

    const ContractSpec& by_id(std::uint32_t id) const {
        auto it = contracts_.find(id);
        if (it == contracts_.end()) throw std::runtime_error("unknown instrument");
        return it->second;
    }

private:
    std::map<std::uint32_t, ContractSpec> contracts_;
};

struct MarketState {
    std::uint32_t active_instrument_id{};
    std::int64_t as_of_ts_ns{};
};

struct Signal {
    std::uint64_t signal_id{};
    std::int64_t ts_decision_ns{};
    std::string root_symbol;
    double target_units{}; // target position, not order quantity
};

struct Order {
    std::uint64_t order_id{};
    std::uint64_t signal_id{};
    std::int64_t ts_created_ns{};
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    Side side{Side::Buy};
    int quantity{};
};

struct RiskDecision {
    RiskVerdict verdict{RiskVerdict::Reject};
    int approved_quantity{};
};

const ContractSpec& resolve_active_contract(
    const std::string& root,
    const MarketState& state,
    const ContractRegistry& registry) {

    const auto& c = registry.by_id(state.active_instrument_id);
    if (c.root_symbol != root) throw std::runtime_error("root mismatch");
    if (!c.is_live_at(state.as_of_ts_ns)) throw std::runtime_error("contract not live");
    return c;
}

Order make_order(
    const Signal& signal,
    int current_position,
    const MarketState& execution_state,
    const ContractRegistry& registry,
    std::uint64_t order_id) {

    double rounded = std::round(signal.target_units);
    if (std::fabs(signal.target_units - rounded) > 1e-6) {
        throw std::runtime_error("target_units must be integral");
    }

    int target = static_cast<int>(rounded);
    int delta = target - current_position;
    if (delta == 0) throw std::runtime_error("no order needed");

    const auto& c = resolve_active_contract(signal.root_symbol, execution_state, registry);

    return {
        order_id,
        signal.signal_id,
        execution_state.as_of_ts_ns,
        c.instrument_id,
        c.raw_symbol,
        delta > 0 ? Side::Buy : Side::Sell,
        std::abs(delta)
    };
}

RiskDecision apply_max_position_risk(const Order& order, int current_position, int max_abs) {
    int signed_delta = (order.side == Side::Buy ? order.quantity : -order.quantity);
    int projected = current_position + signed_delta;

    if (std::abs(projected) <= max_abs) {
        return {RiskVerdict::Approve, order.quantity};
    }

    int allowed_delta = 0;
    if (signed_delta > 0) allowed_delta = max_abs - current_position;
    else allowed_delta = -max_abs - current_position;

    if (allowed_delta == 0 || (allowed_delta > 0) != (signed_delta > 0)) {
        return {RiskVerdict::Reject, 0};
    }

    return {RiskVerdict::Resize, std::abs(allowed_delta)};
}

int main() {
    ContractRegistry registry;
    registry.add({42004058, "NQM6", "NQ", 0.25, 20.0, 1, 9999999999});
    registry.add({42004177, "NQU6", "NQ", 0.25, 20.0, 1, 9999999999});

    Signal sig{1, 100, "NQ", -1.0};

    // Suppose the signal was decided when NQM6 was active,
    // but execution happens after the feed rolled to NQU6.
    MarketState execution_state{42004177, 200};

    int current_position = +1;
    Order order = make_order(sig, current_position, execution_state, registry, 1001);

    std::cout << "Resolved contract: " << order.raw_symbol << '\n';
    std::cout << "Current +1 -> Target -1 => quantity " << order.quantity
              << (order.side == Side::Sell ? " SELL" : " BUY") << '\n';

    auto risk = apply_max_position_risk(order, current_position, 3);
    std::cout << "Risk verdict enum value = " << static_cast<int>(risk.verdict)
              << ", approved quantity = " << risk.approved_quantity << '\n';

    return 0;
}
