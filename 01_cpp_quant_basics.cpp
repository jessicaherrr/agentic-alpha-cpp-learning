#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Lesson 01: C++ building blocks used in a quant trading engine.
// Topics: struct, enum class, const reference, vector, map, optional,
// exceptions, abstract interfaces, and polymorphism.

enum class Side { Buy, Sell };

struct ContractSpec {
    unsigned int instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    double tick_size{};
    double multiplier{};
};

// const ContractSpec& means:
// - do not copy the entire object
// - do not allow this function to modify the object
void print_contract(const ContractSpec& contract) {
    std::cout << contract.raw_symbol
              << " | root=" << contract.root_symbol
              << " | tick=" << contract.tick_size
              << " | multiplier=" << contract.multiplier << '\n';
}

class RiskRule {
public:
    virtual ~RiskRule() = default;
    virtual bool allow(int current_position, int order_delta) const = 0;
};

class MaxPositionRule final : public RiskRule {
public:
    explicit MaxPositionRule(int max_abs_position)
        : max_abs_position_(max_abs_position) {}

    bool allow(int current_position, int order_delta) const override {
        int projected = current_position + order_delta;
        return projected <= max_abs_position_ && projected >= -max_abs_position_;
    }

private:
    int max_abs_position_{};
};

int main() {
    ContractSpec nq{42004177, "NQU6", "NQ", 0.25, 20.0};
    print_contract(nq);

    std::vector<double> closes{30000.0, 30005.0, 30010.0};
    std::cout << "Latest close = " << closes.back() << '\n';

    std::map<std::string, int> positions;
    positions["NQ"] = 1;
    positions["ES"] = -2;
    std::cout << "NQ position = " << positions.at("NQ") << '\n';

    std::optional<double> maybe_fill_price;
    if (!maybe_fill_price) {
        std::cout << "No fill yet.\n";
    }
    maybe_fill_price = 30000.50;
    std::cout << "Fill price = " << *maybe_fill_price << '\n';

    MaxPositionRule rule(3);
    std::cout << "Can current=2 add +1? " << rule.allow(2, 1) << '\n';
    std::cout << "Can current=2 add +2? " << rule.allow(2, 2) << '\n';

    try {
        if (nq.tick_size <= 0.0) {
            throw std::runtime_error("tick_size must be positive");
        }
    } catch (const std::exception& e) {
        std::cerr << "Validation error: " << e.what() << '\n';
    }

    return 0;
}
