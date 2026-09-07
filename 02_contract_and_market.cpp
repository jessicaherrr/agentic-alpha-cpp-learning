#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Lesson 02: Contracts, registry, bars, events, and deterministic ordering.

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
    void add(const ContractSpec& c) {
        contracts_[c.instrument_id] = c;
    }

    const ContractSpec& by_id(std::uint32_t id) const {
        auto it = contracts_.find(id);
        if (it == contracts_.end()) {
            throw std::runtime_error("unknown instrument_id");
        }
        return it->second;
    }

private:
    std::map<std::uint32_t, ContractSpec> contracts_;
};

struct MarketBar {
    std::int64_t ts_event_ns{};
    std::uint32_t instrument_id{};
    double open{};
    double high{};
    double low{};
    double close{};
    std::int64_t volume{};
};

struct MarketEvent {
    std::int64_t ts_event_ns{};
    std::uint64_t seq{};
    std::uint32_t instrument_id{};
    double open{};
    double high{};
    double low{};
    double close{};
    std::int64_t volume{};
};

std::vector<MarketEvent> canonicalize_events(
    const std::vector<MarketBar>& bars,
    const ContractRegistry& registry) {

    std::vector<MarketBar> ordered = bars;

    // Canonical order is independent of caller/file order.
    std::sort(ordered.begin(), ordered.end(), [](const MarketBar& a, const MarketBar& b) {
        if (a.ts_event_ns != b.ts_event_ns) return a.ts_event_ns < b.ts_event_ns;
        return a.instrument_id < b.instrument_id;
    });

    for (std::size_t i = 0; i < ordered.size(); ++i) {
        registry.by_id(ordered[i].instrument_id); // validate instrument
        if (ordered[i].ts_event_ns <= 0) {
            throw std::runtime_error("timestamp must be positive");
        }
        if (i > 0 &&
            ordered[i].ts_event_ns == ordered[i - 1].ts_event_ns &&
            ordered[i].instrument_id == ordered[i - 1].instrument_id) {
            throw std::runtime_error("duplicate bar for instrument/timestamp");
        }
    }

    std::vector<MarketEvent> events;
    events.reserve(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto& b = ordered[i];
        events.push_back({b.ts_event_ns, static_cast<std::uint64_t>(i), b.instrument_id,
                          b.open, b.high, b.low, b.close, b.volume});
    }
    return events;
}

int main() {
    ContractRegistry registry;
    registry.add({700, "ESU6", "ES", 0.25, 50.0, 1, 9999999999});
    registry.add({900, "NQU6", "NQ", 0.25, 20.0, 1, 9999999999});

    std::vector<MarketBar> bars{
        {100, 900, 30000, 30010, 29990, 30005, 1000},
        {100, 700, 6500, 6502, 6498, 6501, 900},
        {200, 900, 30005, 30012, 30000, 30010, 1100},
        {200, 700, 6501, 6503, 6499, 6502, 950},
    };

    auto events = canonicalize_events(bars, registry);

    for (const auto& e : events) {
        const auto& c = registry.by_id(e.instrument_id);
        std::cout << "seq=" << e.seq
                  << " ts=" << e.ts_event_ns
                  << " instrument=" << c.raw_symbol
                  << " close=" << e.close << '\n';
    }

    return 0;
}
