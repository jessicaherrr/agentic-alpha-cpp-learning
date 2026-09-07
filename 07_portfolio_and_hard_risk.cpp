#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

// Lesson 07: Portfolio accounting and hard risk management.
//
// This file is an educational, compact version of the ideas implemented in
// Phase 08 / 08.1 of the production Quant Core.
//
// Core ideas demonstrated here:
//   1. A portfolio is updated from validated Fill events.
//   2. Marks are used for valuation only; marks are NOT execution prices.
//   3. Futures exposure uses price * multiplier * contracts.
//   4. Gross exposure is always non-negative, even if a futures price is negative.
//   5. Hard risk returns APPROVE / RESIZE / REJECT.
//   6. A flip is split into a risk-reducing close leg and a risk-increasing open leg.
//      The close leg is never blocked, but the new opposite position must pass risk.
//
// Compile:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic 07_portfolio_and_hard_risk.cpp -o lesson07
// Run:
//   ./lesson07

enum class Side { Buy, Sell };
enum class RiskVerdict { Approve, Resize, Reject };

struct ContractSpec {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    double multiplier{};
};

struct Fill {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    Side side{Side::Buy};
    int quantity{};
    double fill_price{};
    double multiplier{};
    double commission_usd{};
};

struct Position {
    int units{};                    // signed: +long, -short
    double avg_entry_price{};
    double multiplier{};
    std::string raw_symbol;
    std::string root_symbol;
};

struct Mark {
    double price{};
    std::int64_t ts_ns{};
    bool present{false};
};

struct PositionExposure {
    std::uint32_t instrument_id{};
    int units{};
    double valuation_price{};
    bool valuation_is_estimated{false};
    bool mark_is_stale{true};
    double gross_notional_usd{};
    double signed_notional_usd{};
};

struct PortfolioState {
    double starting_capital_usd{};
    double gross_realized_pnl_usd{};
    double costs_usd{};
    double net_realized_pnl_usd{};
    double unrealized_pnl_usd{};
    double cash_usd{};
    double equity_usd{};

    double gross_exposure_usd{};
    double net_exposure_usd{};
    double long_exposure_usd{};
    double short_exposure_usd{};
    double gross_leverage{};
    double net_leverage{};          // signed reporting ratio

    double initial_margin_usd{};
    double margin_utilization_pct{};

    double peak_equity_usd{};
    double drawdown_usd{};
    double drawdown_pct{};

    bool has_stale_mark{false};
    bool valuation_complete{true};
    std::map<std::uint32_t, PositionExposure> exposures;
};

class MarginModel {
public:
    void set_initial_margin(const std::string& root, double usd_per_contract) {
        if (usd_per_contract < 0.0) {
            throw std::invalid_argument("margin cannot be negative");
        }
        initial_margin_by_root_[root] = usd_per_contract;
    }

    std::optional<double> initial_margin(const std::string& root) const {
        auto it = initial_margin_by_root_.find(root);
        if (it == initial_margin_by_root_.end()) return std::nullopt;
        return it->second;
    }

private:
    std::map<std::string, double> initial_margin_by_root_;
};

class PortfolioAccountant {
public:
    PortfolioAccountant(double starting_capital_usd,
                        const MarginModel& margin_model,
                        std::int64_t mark_staleness_tolerance_ns)
        : starting_capital_usd_(starting_capital_usd),
          margin_model_(margin_model),
          mark_staleness_tolerance_ns_(mark_staleness_tolerance_ns),
          peak_equity_usd_(starting_capital_usd) {
        if (starting_capital_usd <= 0.0) {
            throw std::invalid_argument("starting capital must be positive");
        }
    }

    // Fills are transaction facts. Realized PnL and cash accounting come from fills.
    void apply_fill(const Fill& fill) {
        if (fill.quantity <= 0 || fill.multiplier <= 0.0 || !std::isfinite(fill.fill_price)) {
            throw std::invalid_argument("invalid fill");
        }

        const int signed_qty = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
        Position& p = positions_[fill.instrument_id];

        if (p.units == 0) {
            p.units = signed_qty;
            p.avg_entry_price = fill.fill_price;
            p.multiplier = fill.multiplier;
            p.raw_symbol = fill.raw_symbol;
            p.root_symbol = fill.root_symbol;
            costs_usd_ += fill.commission_usd;
            return;
        }

        if (p.multiplier != fill.multiplier) {
            throw std::runtime_error("multiplier changed for the same instrument");
        }

        // Same direction: add to the position and update weighted average entry.
        if ((p.units > 0 && signed_qty > 0) || (p.units < 0 && signed_qty < 0)) {
            const int old_abs = std::abs(p.units);
            const int add_abs = std::abs(signed_qty);
            p.avg_entry_price =
                (p.avg_entry_price * old_abs + fill.fill_price * add_abs) /
                static_cast<double>(old_abs + add_abs);
            p.units += signed_qty;
            costs_usd_ += fill.commission_usd;
            return;
        }

        // Opposite direction: reduce, close, or flip.
        const int close_qty = std::min(std::abs(p.units), std::abs(signed_qty));
        const double direction = p.units > 0 ? 1.0 : -1.0;
        realized_pnl_usd_ +=
            (fill.fill_price - p.avg_entry_price) * p.multiplier * direction * close_qty;

        const int old_units = p.units;
        p.units += signed_qty;

        // If the trade crossed through zero, the remaining units are a new position.
        if ((old_units > 0 && p.units < 0) || (old_units < 0 && p.units > 0)) {
            p.avg_entry_price = fill.fill_price;
        } else if (p.units == 0) {
            p.avg_entry_price = 0.0;
        }

        costs_usd_ += fill.commission_usd;
    }

    // Marks are valuation observations. They never become fills in this class.
    void observe_mark(std::uint32_t instrument_id, double price, std::int64_t ts_ns) {
        if (!std::isfinite(price)) throw std::invalid_argument("mark must be finite");
        marks_[instrument_id] = Mark{price, ts_ns, true};
    }

    int units(std::uint32_t instrument_id) const {
        auto it = positions_.find(instrument_id);
        return it == positions_.end() ? 0 : it->second.units;
    }

    PortfolioState snapshot(std::int64_t as_of_ts_ns) {
        PortfolioState s;
        s.starting_capital_usd = starting_capital_usd_;
        s.gross_realized_pnl_usd = realized_pnl_usd_;
        s.costs_usd = costs_usd_;
        s.net_realized_pnl_usd = realized_pnl_usd_ - costs_usd_;
        s.cash_usd = starting_capital_usd_ + s.net_realized_pnl_usd;

        for (const auto& [id, p] : positions_) {
            if (p.units == 0) continue;

            PositionExposure e;
            e.instrument_id = id;
            e.units = p.units;

            auto mit = marks_.find(id);
            if (mit != marks_.end() && mit->second.present) {
                e.valuation_price = mit->second.price;
                const std::int64_t age = std::max<std::int64_t>(0, as_of_ts_ns - mit->second.ts_ns);
                e.mark_is_stale = age > mark_staleness_tolerance_ns_;
            } else {
                // Educational fallback only: use entry price as an estimate.
                // It is explicitly flagged as incomplete/stale and should block
                // risk-increasing orders under the default risk policy.
                e.valuation_price = p.avg_entry_price;
                e.valuation_is_estimated = true;
                e.mark_is_stale = true;
            }

            if (e.mark_is_stale || e.valuation_is_estimated) {
                s.has_stale_mark = true;
                s.valuation_complete = false;
            }

            // Important for signed futures prices:
            // gross notional is a magnitude; direction comes from POSITION units,
            // not from the sign of the market price.
            e.gross_notional_usd =
                std::abs(static_cast<double>(p.units) * e.valuation_price * p.multiplier);
            e.signed_notional_usd = p.units > 0 ? e.gross_notional_usd : -e.gross_notional_usd;

            s.gross_exposure_usd += e.gross_notional_usd;
            s.net_exposure_usd += e.signed_notional_usd;
            if (p.units > 0) s.long_exposure_usd += e.gross_notional_usd;
            if (p.units < 0) s.short_exposure_usd += e.gross_notional_usd;

            // Margin is metadata, not guessed from notional.
            if (auto m = margin_model_.initial_margin(p.root_symbol)) {
                s.initial_margin_usd += std::abs(p.units) * (*m);
            }

            // Mark-to-market PnL keeps the true signed price difference.
            s.unrealized_pnl_usd +=
                (e.valuation_price - p.avg_entry_price) * p.multiplier * p.units;

            s.exposures[id] = e;
        }

        s.equity_usd = s.cash_usd + s.unrealized_pnl_usd;
        if (s.equity_usd > 0.0) {
            s.gross_leverage = s.gross_exposure_usd / s.equity_usd;
            s.net_leverage = s.net_exposure_usd / s.equity_usd;
            s.margin_utilization_pct = s.initial_margin_usd / s.equity_usd;
        } else if (s.initial_margin_usd > 0.0) {
            s.margin_utilization_pct = 1e9; // finite sentinel for reporting
        }

        peak_equity_usd_ = std::max(peak_equity_usd_, s.equity_usd);
        s.peak_equity_usd = peak_equity_usd_;
        s.drawdown_usd = std::max(0.0, peak_equity_usd_ - s.equity_usd);
        if (peak_equity_usd_ > 0.0) {
            s.drawdown_pct = s.drawdown_usd / peak_equity_usd_;
        }
        return s;
    }

private:
    double starting_capital_usd_{};
    const MarginModel& margin_model_;
    std::int64_t mark_staleness_tolerance_ns_{};

    std::map<std::uint32_t, Position> positions_;
    std::map<std::uint32_t, Mark> marks_;
    double realized_pnl_usd_{0.0};
    double costs_usd_{0.0};
    double peak_equity_usd_{0.0};
};

struct Order {
    std::uint32_t instrument_id{};
    std::string raw_symbol;
    std::string root_symbol;
    Side side{Side::Buy};
    int quantity{};
    double reference_price{};
    double multiplier{};
};

struct RiskDecision {
    RiskVerdict verdict{RiskVerdict::Reject};
    int requested_quantity{};
    int approved_quantity{};
    std::string reason;
};

struct RiskConfig {
    int max_abs_contracts_per_symbol{3};
    int max_order_open_contracts{3};
    double max_gross_leverage{5.0};
    double max_net_leverage{4.0};
    double max_margin_utilization_pct{0.75};
    double max_drawdown_pct{0.20};
    bool reject_risk_increasing_on_stale_mark{true};
};

class HardRiskManager {
public:
    HardRiskManager(RiskConfig config, const MarginModel& margin_model)
        : config_(config), margin_model_(margin_model) {}

    RiskDecision review(const Order& order,
                        int current_units,
                        const PortfolioState& current_state) const {
        if (order.quantity <= 0) {
            return {RiskVerdict::Reject, order.quantity, 0, "non_positive_quantity"};
        }

        const int order_sign = order.side == Side::Buy ? 1 : -1;
        const bool opposes_position =
            (current_units > 0 && order_sign < 0) || (current_units < 0 && order_sign > 0);

        // The part that moves the existing position toward zero.
        const int close_qty = opposes_position
            ? std::min(order.quantity, std::abs(current_units))
            : 0;

        // Only this remainder creates new or larger exposure.
        const int requested_open = order.quantity - close_qty;

        if (requested_open == 0) {
            return {RiskVerdict::Approve, order.quantity, order.quantity, "risk_reducing_only"};
        }

        // Kill switches and stale-data rules may block NEW exposure,
        // but they must not block close_qty.
        if (current_state.drawdown_pct >= config_.max_drawdown_pct) {
            return resize_to_close(order, close_qty, "drawdown_kill_switch");
        }
        if (config_.reject_risk_increasing_on_stale_mark && current_state.has_stale_mark) {
            return resize_to_close(order, close_qty, "stale_mark");
        }

        if (!margin_model_.initial_margin(order.root_symbol).has_value()) {
            return resize_to_close(order, close_qty, "missing_margin");
        }

        // Search for the largest OPENING quantity that fits all limits.
        const int max_open_request = std::min(requested_open, config_.max_order_open_contracts);
        for (int open_qty = max_open_request; open_qty >= 0; --open_qty) {
            const int total_approved = close_qty + open_qty;
            if (total_approved == 0) continue;

            const int post_units = current_units + order_sign * total_approved;
            if (std::abs(post_units) > config_.max_abs_contracts_per_symbol) continue;

            const double gross_for_symbol =
                std::abs(static_cast<double>(post_units) * order.reference_price * order.multiplier);

            // A compact educational approximation of post-trade portfolio exposure:
            // remove current instrument's old exposure if present, then add post-trade exposure.
            double current_symbol_gross = 0.0;
            auto eit = current_state.exposures.find(order.instrument_id);
            if (eit != current_state.exposures.end()) {
                current_symbol_gross = eit->second.gross_notional_usd;
            }
            const double post_gross =
                std::max(0.0, current_state.gross_exposure_usd - current_symbol_gross) + gross_for_symbol;

            const double signed_symbol = post_units == 0
                ? 0.0
                : (post_units > 0 ? gross_for_symbol : -gross_for_symbol);
            double current_symbol_signed = 0.0;
            if (eit != current_state.exposures.end()) {
                current_symbol_signed = eit->second.signed_notional_usd;
            }
            const double post_net = current_state.net_exposure_usd - current_symbol_signed + signed_symbol;

            const double equity = current_state.equity_usd;
            if (equity <= 0.0) continue;
            if (post_gross / equity > config_.max_gross_leverage) continue;
            if (std::abs(post_net / equity) > config_.max_net_leverage) continue;

            const double margin_per_contract = *margin_model_.initial_margin(order.root_symbol);
            const double post_margin = current_state.initial_margin_usd
                - std::abs(current_units) * margin_per_contract
                + std::abs(post_units) * margin_per_contract;
            if (post_margin / equity > config_.max_margin_utilization_pct) continue;

            if (total_approved == order.quantity) {
                return {RiskVerdict::Approve, order.quantity, total_approved, "approved"};
            }
            return {RiskVerdict::Resize, order.quantity, total_approved, "risk_limit_resize"};
        }

        return resize_to_close(order, close_qty, "no_open_quantity_feasible");
    }

private:
    static RiskDecision resize_to_close(const Order& order,
                                        int close_qty,
                                        const std::string& reason) {
        if (close_qty > 0) {
            // The close portion survives even when all new exposure is blocked.
            if (close_qty == order.quantity) {
                return {RiskVerdict::Approve, order.quantity, close_qty, reason};
            }
            return {RiskVerdict::Resize, order.quantity, close_qty, reason};
        }
        return {RiskVerdict::Reject, order.quantity, 0, reason};
    }

    RiskConfig config_;
    const MarginModel& margin_model_;
};

std::string verdict_name(RiskVerdict v) {
    switch (v) {
        case RiskVerdict::Approve: return "APPROVE";
        case RiskVerdict::Resize: return "RESIZE";
        case RiskVerdict::Reject: return "REJECT";
    }
    return "UNKNOWN";
}

int main() {
    std::cout << "=== Lesson 07: Portfolio + Hard Risk ===\n\n";

    MarginModel margins;
    margins.set_initial_margin("NQ", 22000.0); // educational example only
    margins.set_initial_margin("CL", 9000.0);  // educational example only

    PortfolioAccountant portfolio(100000.0, margins, 60'000'000'000LL); // 60 seconds

    // Open +2 NQ at 20,000.
    portfolio.apply_fill({42004177, "NQU6", "NQ", Side::Buy, 2, 20000.0, 20.0, 4.0});
    portfolio.observe_mark(42004177, 20050.0, 1'000'000'000LL);

    PortfolioState state = portfolio.snapshot(1'000'000'000LL);
    std::cout << "Cash:              $" << state.cash_usd << '\n';
    std::cout << "Unrealized PnL:    $" << state.unrealized_pnl_usd << '\n';
    std::cout << "Equity:            $" << state.equity_usd << '\n';
    std::cout << "Gross exposure:    $" << state.gross_exposure_usd << '\n';
    std::cout << "Gross leverage:     " << state.gross_leverage << 'x' << '\n';
    std::cout << "Margin utilization: " << state.margin_utilization_pct * 100.0 << "%\n\n";

    RiskConfig risk_config;
    risk_config.max_abs_contracts_per_symbol = 3;
    risk_config.max_order_open_contracts = 3;
    risk_config.max_gross_leverage = 10.0;
    risk_config.max_net_leverage = 10.0;
    risk_config.max_margin_utilization_pct = 1.0;
    risk_config.max_drawdown_pct = 0.20;

    HardRiskManager risk(risk_config, margins);

    // Current position is +2 NQ. SELL 5 means:
    //   SELL 2 -> close the existing long (always allowed)
    //   SELL 3 -> request a new short position (must pass risk)
    Order flip{42004177, "NQU6", "NQ", Side::Sell, 5, 20050.0, 20.0};
    RiskDecision d = risk.review(flip, portfolio.units(42004177), state);

    std::cout << "Flip example: current +2, request SELL 5\n";
    std::cout << "Decision: " << verdict_name(d.verdict)
              << " approved=" << d.approved_quantity
              << " / requested=" << d.requested_quantity
              << " reason=" << d.reason << "\n\n";

    // Demonstrate the signed-price exposure rule with CL.
    // A LONG position is still LONG even if the market price is negative.
    PortfolioAccountant cl_portfolio(100000.0, margins, 0);
    cl_portfolio.apply_fill({777, "CLM20", "CL", Side::Buy, 1, -15.0, 1000.0, 2.0});
    cl_portfolio.observe_mark(777, -20.0, 10);
    PortfolioState cl_state = cl_portfolio.snapshot(10);

    const auto& cl_exp = cl_state.exposures.at(777);
    std::cout << "Negative-price CL example:\n";
    std::cout << "  units                 = " << cl_exp.units << " (LONG)\n";
    std::cout << "  mark price            = " << cl_exp.valuation_price << '\n';
    std::cout << "  gross exposure        = $" << cl_exp.gross_notional_usd << '\n';
    std::cout << "  signed/net exposure   = $" << cl_exp.signed_notional_usd << '\n';
    std::cout << "  unrealized PnL        = $" << cl_state.unrealized_pnl_usd << '\n';

    return 0;
}
