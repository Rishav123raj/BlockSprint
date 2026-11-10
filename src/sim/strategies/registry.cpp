#include "strategies/registry.hpp"

#include "strategies/params.hpp"
#include "strategies/passive_quoter.hpp"
#include "strategies/twap_taker.hpp"

#include <functional>
#include <map>
#include <stdexcept>

namespace sim {

namespace {

// Never trades
class NullStrategy final : public Strategy {
public:
    std::string name() const override { return "null"; }
};

using Factory = std::function<std::unique_ptr<Strategy>(StrategyParams&)>;

const std::map<std::string, Factory>& factories() {
    static const std::map<std::string, Factory> f{
        {"null", [](StrategyParams&) { return std::make_unique<NullStrategy>(); }},
        {"passive_quoter",
         [](StrategyParams& p) { return std::make_unique<PassiveQuoter>(PassiveQuoter::config_from(p)); }},
        {"twap_taker", [](StrategyParams& p) { return std::make_unique<TwapTaker>(TwapTaker::config_from(p)); }},
    };
    return f;
}

} // namespace

std::unique_ptr<Strategy> make_strategy(const std::string& name, const nlohmann::json& params) {
    auto it = factories().find(name);
    if (it == factories().end()) {
        std::string names;
        for (const auto& n : strategy_names()) names += (names.empty() ? "" : ", ") + n;
        throw std::invalid_argument("strategy.name: unknown strategy '" + name + "' (" + names + ")");
    }
    StrategyParams p(params, name);
    auto strategy = it->second(p);
    p.done();
    return strategy;
}

std::vector<std::string> strategy_names() {
    std::vector<std::string> names;
    for (const auto& [name, factory] : factories()) names.push_back(name);
    return names;
}

} // namespace sim
