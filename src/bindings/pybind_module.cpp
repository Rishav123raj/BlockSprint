// blocksprint_sim: the simulator as a Python module (Phase 4, docs/dev_setup.md).
//
// P4.1: the run-config path. A config is a Python dict in the run-config format (configs/sim/*.json,
// docs/simulator_architecture.md section 13.1), resolved exactly as `bs_sim run` resolves it, and run with
// run_simulation, the same call the CLI makes.
//
// P4.3: SimConfig (a resolved config) and Simulator (a run, with the config's C++ strategy or a Python one).
// P4.4: Python strategies. A Python object with any of on_start / on_book / on_trade / on_exec / on_reject /
// on_timer / on_gap / on_end is driven by a C++ adapter. The GIL is released for the whole replay and taken
// only to call a method the strategy defines; callback_mode filters book and trade callbacks in C++ before
// the GIL is taken:
//   every_event  every depth event / snapshot (on_book) and every trade (on_trade)
//   on_change    on_book only when the best bid or ask (price or quantity) changed; every trade
//   timer_only   no on_book / on_trade (the book is still kept: read ctx.book() in on_timer)
// on_start, on_exec, on_reject, on_timer, on_gap and on_end are always delivered.
//
// Prices and quantities are integers in 1e-8 units, as in C++ (exact; to_fixed / from_fixed convert).
// The Context and ObservedBook passed to a callback are valid during that callback only.
//
// P4.5: Simulator.tables() returns the run's outputs as numpy structured arrays that view the recorder's
// rows (no copy; each array keeps the rows alive). result_codes() names the enum codes with the CSV's
// strings. research/sim/results.py wraps both (Results, to_polars).
#include "config.hpp"
#include "run.hpp"
#include "strategies/registry.hpp"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;
using nlohmann::json;
using namespace sim;

namespace {

// Python objects cross as JSON text: exact for everything a config holds (strings, integers, floats,
// lists, dicts), and the C++ side keeps a single parser
json to_json(const py::handle& obj) {
    if (py::isinstance<py::str>(obj)) {
        return json::parse(obj.cast<std::string>());  // a config given as JSON text
    }
    const auto text = py::module_::import("json").attr("dumps")(obj).cast<std::string>();
    return json::parse(text);
}

py::object to_python(const json& j) { return py::module_::import("json").attr("loads")(j.dump()); }

RunConfig resolve(const py::handle& config, const std::vector<std::string>& overrides) {
    return resolve_run_config(to_json(config), overrides);
}

int64_t to_fixed(const py::object& value) {
    if (py::isinstance<py::int_>(value)) {
        return value.cast<int64_t>() * kFixedScale;
    }
    const std::string text = py::str(value).cast<std::string>();
    const bool negative = !text.empty() && text[0] == '-';
    const auto parsed = parse_fixed(negative ? text.substr(1) : text);
    if (!parsed) {
        throw std::invalid_argument("not a decimal with at most 8 places: " + text);
    }
    return negative ? -*parsed : *parsed;
}

enum class CallbackMode { EVERY_EVENT, ON_CHANGE, TIMER_ONLY };

CallbackMode callback_mode(const std::string& name) {
    if (name == "every_event") return CallbackMode::EVERY_EVENT;
    if (name == "on_change") return CallbackMode::ON_CHANGE;
    if (name == "timer_only") return CallbackMode::TIMER_ONLY;
    throw std::invalid_argument("callback_mode: expected every_event, on_change or timer_only, got '" + name + "'");
}

// Drives a Python strategy object. Built and destroyed with the GIL held; between them it runs inside the
// replay, which holds no GIL.
class PyStrategy final : public Strategy {
public:
    PyStrategy(py::object obj, CallbackMode mode) : obj_(std::move(obj)), mode_(mode) {
        start_ = method("on_start");
        book_ = method("on_book");
        trade_ = method("on_trade");
        exec_ = method("on_exec");
        reject_ = method("on_reject");
        timer_ = method("on_timer");
        gap_ = method("on_gap");
        end_ = method("on_end");
        py::object n = py::getattr(obj_, "name", py::none());
        if (!n.is_none() && PyCallable_Check(n.ptr())) n = n();
        name_ = n.is_none() ? py::str(py::type::of(obj_).attr("__name__")).cast<std::string>()
                            : py::str(n).cast<std::string>();
    }

    std::string name() const override { return name_; }
    uint64_t callbacks() const { return callbacks_; }

    void on_start(Context& ctx) override { call(start_, ctx); }
    void on_end(Context& ctx) override { call(end_, ctx); }
    void on_gap(Context& ctx) override { call(gap_, ctx); }

    void on_book(Context& ctx, const ObservedBook& book) override {
        if (!book_ || mode_ == CallbackMode::TIMER_ONLY) return;
        if (mode_ == CallbackMode::ON_CHANGE) {
            const auto bid = book.best_bid(), ask = book.best_ask();
            if (seen_top_ && bid == last_bid_ && ask == last_ask_) return;
            seen_top_ = true;
            last_bid_ = bid;
            last_ask_ = ask;
        }
        py::gil_scoped_acquire gil;
        ++callbacks_;
        book_(py::cast(&ctx, py::return_value_policy::reference), py::cast(&book, py::return_value_policy::reference));
    }

    void on_trade(Context& ctx, const TradeEvent& t) override {
        if (!trade_ || mode_ == CallbackMode::TIMER_ONLY) return;
        py::gil_scoped_acquire gil;
        ++callbacks_;
        trade_(py::cast(&ctx, py::return_value_policy::reference), t);
    }

    void on_exec(Context& ctx, const ExecReport& r) override {
        if (!exec_) return;
        py::gil_scoped_acquire gil;
        ++callbacks_;
        exec_(py::cast(&ctx, py::return_value_policy::reference), r);
    }

    void on_reject(Context& ctx, const RequestReject& r) override {
        if (!reject_) return;
        py::gil_scoped_acquire gil;
        ++callbacks_;
        reject_(py::cast(&ctx, py::return_value_policy::reference), r);
    }

    void on_timer(Context& ctx, uint64_t id) override {
        if (!timer_) return;
        py::gil_scoped_acquire gil;
        ++callbacks_;
        timer_(py::cast(&ctx, py::return_value_policy::reference), id);
    }

private:
    // The bound method, or null when absent or a base-class default (marked _bs_default, P4.6)
    py::object method(const char* name) {
        py::object m = py::getattr(obj_, name, py::none());
        if (m.is_none() || !PyCallable_Check(m.ptr()) || py::hasattr(m, "_bs_default")) return py::object();
        return m;
    }

    void call(const py::object& fn, Context& ctx) {
        if (!fn) return;
        py::gil_scoped_acquire gil;
        ++callbacks_;
        fn(py::cast(&ctx, py::return_value_policy::reference));
    }

    py::object obj_;
    CallbackMode mode_;
    std::string name_;
    py::object start_, book_, trade_, exec_, reject_, timer_, gap_, end_;
    bool seen_top_ = false;
    std::optional<BookLevel> last_bid_, last_ask_;
    uint64_t callbacks_ = 0;  // read with the GIL held, after the run
};

// A run: the config, and optionally a Python strategy in place of the config's C++ one
// A numpy view of rows that live in tables: the array's base holds a reference to the tables
template <class Row>
py::array view(const std::shared_ptr<ResultTables>& tables, const std::vector<Row>& rows) {
    auto* keep = new std::shared_ptr<ResultTables>(tables);
    py::capsule base(keep, [](void* p) { delete static_cast<std::shared_ptr<ResultTables>*>(p); });
    return py::array_t<Row>({static_cast<py::ssize_t>(rows.size())}, {static_cast<py::ssize_t>(sizeof(Row))},
                            rows.data(), base);
}

py::dict tables_dict(const std::shared_ptr<ResultTables>& t) {
    py::dict d;
    d["owners"] = t->owners;
    d["order_ids"] = t->order_ids;
    d["orders"] = view(t, t->orders);
    d["exec_reports"] = view(t, t->reports);
    d["request_rejects"] = view(t, t->rejects);
    d["reject_texts"] = t->reject_texts;
    d["bbo_samples"] = view(t, t->bbo);
    d["positions"] = view(t, t->positions);
    return d;
}

// Code -> the CSV's string, per enum column of the result tables
py::dict result_codes() {
    py::dict d;
    std::vector<std::string> v;
    for (Side x : {Side::BUY, Side::SELL}) v.push_back(sideToString(x));
    d["side"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(OrderType::LIMIT_MAKER); ++c) v.push_back(orderTypeToString(OrderType(c)));
    d["order_type"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(StpPolicy::CANCEL_BOTH); ++c) v.push_back(stpPolicyToString(StpPolicy(c)));
    d["stp"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(Request::Type::REPLACE); ++c) v.push_back(request_type_name(Request::Type(c)));
    d["request"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(ExecType::REJECTED); ++c) v.push_back(execTypeToString(ExecType(c)));
    d["type"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(ExecReason::MAX_OPEN_ORDERS); ++c) {
        v.push_back(execReasonToString(ExecReason(c)));
    }
    d["reason"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(Liquidity::TAKER); ++c) v.push_back(liquidityToString(Liquidity(c)));
    d["liquidity"] = v;
    v.clear();
    for (int c = 0; c <= static_cast<int>(RequestReject::Request::REPLACE); ++c) {
        v.push_back(request_name(RequestReject::Request(c)));
    }
    d["reject_request"] = v;
    return d;
}

struct PySimulator {
    RunConfig rc;
    py::object strategy;  // None: strategy.name from the registry
    CallbackMode mode;
    bool ran = false;
    py::object meta = py::none();
    std::shared_ptr<ResultTables> tables;

    py::object run() {
        if (ran) {
            throw std::logic_error("Simulator.run: a simulator runs once (make a new one)");
        }
        ran = true;
        json extra;
        extra["run"] = {{"command", "blocksprint_sim.Simulator.run"}};
        std::optional<PyStrategy> adapter;
        if (!strategy.is_none()) {
            adapter.emplace(strategy, mode);
            extra["python_strategy"] = adapter->name();
        }
        RunOutcome outcome;
        {
            py::gil_scoped_release release;  // the adapter takes it back for each Python callback
            outcome = run_simulation(rc, extra, adapter ? &*adapter : nullptr, /*keep_tables=*/true);
        }
        meta = to_python(outcome.meta);
        if (adapter) {
            meta["python_callbacks"] = adapter->callbacks();
        }
        tables = std::move(outcome.tables);
        return meta;
    }

    py::dict get_tables() const {
        if (!tables) {
            throw std::logic_error("Simulator.tables: run() first");
        }
        return tables_dict(tables);
    }
};

std::string repr_fixed(int64_t v) { return format_fixed(v); }

} // namespace

PYBIND11_MODULE(blocksprint_sim, m) {
    m.doc() = "BlockSprint market simulator: replay recorded Binance data through the matching engine with "
              "latency and queue models (docs/simulator_architecture.md, docs/dev_setup.md)";
    m.attr("__version__") = BLOCKSPRINT_VERSION;
    m.attr("FIXED_SCALE") = kFixedScale;

    // numpy dtypes of the result rows (recorder.hpp); padding is left out of the fields, kept in the itemsize
    PYBIND11_NUMPY_DTYPE(OrderRow, sent_ns, arrive_ns, owner, order, request, side, order_type, stp, has_new_price, price,
                             quantity, new_price);
    PYBIND11_NUMPY_DTYPE(ReportRow, exchange_ns, deliver_ns, seq, owner, order, type, reason, side, order_type, liquidity,
                             price, quantity, last_px, last_qty, cum_qty, leaves_qty, fee, cum_fee);
    PYBIND11_NUMPY_DTYPE(RejectRow, exchange_ns, deliver_ns, owner, order, request, reason);
    PYBIND11_NUMPY_DTYPE(BboRow, ts_ns, owner, data_bid, data_bid_qty, data_ask, data_ask_qty, book_bid, book_bid_qty,
                             book_ask, book_ask_qty, seen_bid, seen_bid_qty, seen_ask, seen_ask_qty, seen_data_ns);
    PYBIND11_NUMPY_DTYPE(PositionRow, ts_ns, owner, position, cash, fees, mark, pnl, fills, maker_fills, taker_fills,
                             open_orders, base_volume, quote_volume);


    // std::invalid_argument (bad config keys, unknown strategy or parameter) arrives as ValueError;
    // std::logic_error (e.g. trading during the warm-up) as RuntimeError

    // ---- values ----
    m.def("to_fixed", &to_fixed, py::arg("value"),
          "A decimal (str such as '0.001', or an int) as an integer in 1e-8 units: to_fixed('100.01') == "
          "10001000000. Exact; floats are not accepted as exact input, pass a string.");
    m.def("from_fixed", &repr_fixed, py::arg("value"), "An integer in 1e-8 units as a decimal string");

    py::enum_<Side>(m, "Side").value("BUY", Side::BUY).value("SELL", Side::SELL);
    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::LIMIT)
        .value("MARKET", OrderType::MARKET)
        .value("IOC", OrderType::IOC)
        .value("FOK", OrderType::FOK)
        .value("LIMIT_MAKER", OrderType::LIMIT_MAKER);
    py::enum_<StpPolicy>(m, "StpPolicy")
        .value("NONE", StpPolicy::NONE)
        .value("SKIP", StpPolicy::SKIP)
        .value("CANCEL_TAKER", StpPolicy::CANCEL_TAKER)
        .value("CANCEL_MAKER", StpPolicy::CANCEL_MAKER)
        .value("CANCEL_BOTH", StpPolicy::CANCEL_BOTH);
    py::enum_<ExecType>(m, "ExecType")
        .value("ACK", ExecType::ACK)
        .value("FILL", ExecType::FILL)
        .value("AMENDED", ExecType::AMENDED)
        .value("CANCELLED", ExecType::CANCELLED)
        .value("REJECTED", ExecType::REJECTED);
    py::enum_<ExecReason>(m, "ExecReason")
        .value("NONE", ExecReason::NONE)
        .value("USER", ExecReason::USER)
        .value("REPLACED", ExecReason::REPLACED)
        .value("IOC_REMAINDER", ExecReason::IOC_REMAINDER)
        .value("NO_LIQUIDITY", ExecReason::NO_LIQUIDITY)
        .value("FOK_KILL", ExecReason::FOK_KILL)
        .value("WOULD_CROSS", ExecReason::WOULD_CROSS)
        .value("STP_TAKER", ExecReason::STP_TAKER)
        .value("STP_MAKER", ExecReason::STP_MAKER)
        .value("DUPLICATE_ORDER_ID", ExecReason::DUPLICATE_ORDER_ID)
        .value("INVALID_QUANTITY", ExecReason::INVALID_QUANTITY)
        .value("INVALID_PRICE", ExecReason::INVALID_PRICE)
        .value("PRICE_TICK", ExecReason::PRICE_TICK)
        .value("QUANTITY_LOT", ExecReason::QUANTITY_LOT)
        .value("QUANTITY_RANGE", ExecReason::QUANTITY_RANGE)
        .value("MIN_NOTIONAL", ExecReason::MIN_NOTIONAL)
        .value("POST_ONLY_WOULD_TAKE", ExecReason::POST_ONLY_WOULD_TAKE)
        .value("RATE_LIMIT", ExecReason::RATE_LIMIT)
        .value("MAX_OPEN_ORDERS", ExecReason::MAX_OPEN_ORDERS);
    py::enum_<Liquidity>(m, "Liquidity")
        .value("NONE", Liquidity::NONE)
        .value("MAKER", Liquidity::MAKER)
        .value("TAKER", Liquidity::TAKER);
    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("PENDING_NEW", OrderStatus::PENDING_NEW)
        .value("NEW", OrderStatus::NEW)
        .value("PARTIALLY_FILLED", OrderStatus::PARTIALLY_FILLED)
        .value("PENDING_CANCEL", OrderStatus::PENDING_CANCEL)
        .value("PENDING_REPLACE", OrderStatus::PENDING_REPLACE);
    py::enum_<RequestReject::Request>(m, "Request")
        .value("CANCEL", RequestReject::Request::CANCEL)
        .value("AMEND", RequestReject::Request::AMEND)
        .value("REPLACE", RequestReject::Request::REPLACE);

    // ---- what a strategy sees ----
    py::class_<BookLevel>(m, "BookLevel")
        .def_readonly("price", &BookLevel::price)
        .def_readonly("qty", &BookLevel::qty)
        .def("__eq__", [](const BookLevel& a, const BookLevel& b) { return a == b; })
        .def("__repr__", [](const BookLevel& l) {
            return "BookLevel(" + format_fixed(l.price) + " x " + format_fixed(l.qty) + ")";
        });

    py::class_<TradeEvent>(m, "TradeEvent")
        .def_readonly("ts_local", &TradeEvent::ts_local)
        .def_readonly("ts_exch", &TradeEvent::ts_exch)
        .def_readonly("price", &TradeEvent::price)
        .def_readonly("qty", &TradeEvent::qty)
        .def_readonly("aggressor", &TradeEvent::aggressor)
        .def("__repr__", [](const TradeEvent& t) {
            return std::string("TradeEvent(") + (t.aggressor == Side::BUY ? "buy " : "sell ") + format_fixed(t.qty) +
                   " @ " + format_fixed(t.price) + ")";
        });

    py::class_<ObservedBook>(m, "ObservedBook",
                             "The market as the strategy sees it, after feed latency (valid during the callback)")
        .def("valid", &ObservedBook::valid, "A snapshot has established the book and no gap has happened since")
        .def("best_bid", &ObservedBook::best_bid)
        .def("best_ask", &ObservedBook::best_ask)
        .def("qty", &ObservedBook::qty, py::arg("side"), py::arg("price"))
        .def(
            "top",
            [](const ObservedBook& b, Side side, size_t n) {
                std::vector<BookLevel> out;
                b.top(side, n, out);
                return out;
            },
            py::arg("side"), py::arg("n"), "The best n levels of a side, best first")
        .def("levels", &ObservedBook::levels, py::arg("side"))
        .def("last_trade", &ObservedBook::last_trade)
        .def("data_time", &ObservedBook::data_time, "ts_local of the newest batch applied")
        .def("seen_at", &ObservedBook::seen_at, "Sim time the newest batch reached the strategy")
        .def_property_readonly("tick_size", &ObservedBook::tick_size)
        .def_property_readonly("lot_size", &ObservedBook::lot_size);

    py::class_<ExecReport>(m, "ExecReport")
        .def_readonly("seq", &ExecReport::seq)
        .def_readonly("timestamp", &ExecReport::timestamp)
        .def_readonly("type", &ExecReport::type)
        .def_readonly("reason", &ExecReport::reason)
        .def_readonly("text", &ExecReport::text)
        .def_readonly("order_id", &ExecReport::order_id)
        .def_readonly("owner_id", &ExecReport::owner_id)
        .def_readonly("symbol", &ExecReport::symbol)
        .def_readonly("side", &ExecReport::side)
        .def_readonly("order_type", &ExecReport::order_type)
        .def_readonly("price", &ExecReport::price)
        .def_readonly("quantity", &ExecReport::quantity)
        .def_readonly("cum_qty", &ExecReport::cum_qty)
        .def_readonly("leaves_qty", &ExecReport::leaves_qty)
        .def_readonly("cum_fee", &ExecReport::cum_fee)
        .def_readonly("last_px", &ExecReport::last_px)
        .def_readonly("last_qty", &ExecReport::last_qty)
        .def_readonly("liquidity", &ExecReport::liquidity)
        .def_readonly("trade_id", &ExecReport::trade_id)
        .def_readonly("fee", &ExecReport::fee)
        .def("__repr__", [](const ExecReport& r) {
            return "ExecReport(" + execTypeToString(r.type) + " " + r.order_id + ")";
        });

    py::class_<RequestReject>(m, "RequestReject")
        .def_readonly("request", &RequestReject::request)
        .def_readonly("order_id", &RequestReject::order_id)
        .def_readonly("reason", &RequestReject::reason)
        .def_readonly("text", &RequestReject::text)
        .def_readonly("exchange_time", &RequestReject::exchange_time);

    py::class_<Account>(m, "Account")
        .def_readonly("position", &Account::position)
        .def_readonly("cash", &Account::cash)
        .def_readonly("fees", &Account::fees)
        .def_readonly("base_volume", &Account::base_volume)
        .def_readonly("quote_volume", &Account::quote_volume)
        .def_readonly("fills", &Account::fills)
        .def_readonly("maker_fills", &Account::maker_fills)
        .def_readonly("taker_fills", &Account::taker_fills)
        .def("pnl", &Account::pnl, py::arg("mark"), "cash + position x mark - fees, in 1e-8 quote units");

    py::class_<OpenOrder>(m, "OpenOrder")
        .def_readonly("id", &OpenOrder::id)
        .def_readonly("side", &OpenOrder::side)
        .def_readonly("type", &OpenOrder::type)
        .def_readonly("price", &OpenOrder::price)
        .def_readonly("quantity", &OpenOrder::quantity)
        .def_readonly("filled", &OpenOrder::filled)
        .def_readonly("leaves", &OpenOrder::leaves)
        .def_readonly("status", &OpenOrder::status)
        .def_readonly("sent_at", &OpenOrder::sent_at);

    py::class_<Context>(m, "Context", "A strategy's connection to the exchange (valid during the callback)")
        .def("now", &Context::now, "Sim time of the callback (ns since the epoch)")
        .def("symbol", &Context::symbol)
        .def("owner", &Context::owner)
        .def("book", &Context::book, py::return_value_policy::reference)
        .def("can_trade", &Context::can_trade, "False during the warm-up: requests raise RuntimeError then")
        .def("send", &Context::send, py::arg("side"), py::arg("type"), py::arg("price"), py::arg("quantity"),
             py::arg("stp") = StpPolicy::CANCEL_MAKER, "Sends a new order; returns its ID")
        .def("cancel", &Context::cancel, py::arg("order_id"))
        .def("amend", &Context::amend, py::arg("order_id"), py::arg("new_total"), py::arg("new_price") = py::none(),
             "New total quantity (filled + open), optionally a new price")
        .def("replace", &Context::replace, py::arg("order_id"), py::arg("price"), py::arg("quantity"),
             "Cancel and send under the same ID (loses queue priority)")
        .def(
            "account", [](const Context& c) { return c.account(); }, "The account as the strategy knows it (a copy)")
        .def(
            "open_orders", [](const Context& c) { return c.open_orders(); }, "Order ID -> OpenOrder (a copy)")
        .def("set_timer", &Context::set_timer, py::arg("delay_ns"), "on_timer after delay_ns; returns the timer ID")
        .def("cancel_timer", &Context::cancel_timer, py::arg("timer_id"));

    // ---- runs ----
    m.def("strategy_names", &strategy_names, "Names accepted by strategy.name (C++ strategies)");
    m.def("latency_presets", &latency_preset_names, "Names accepted by latency_preset");

    m.def(
        "resolve_config",
        [](const py::object& config, const std::vector<std::string>& overrides) {
            return to_python(resolve(config, overrides).resolved);
        },
        py::arg("config"), py::arg("overrides") = std::vector<std::string>{},
        "The fully resolved run config (defaults < latency preset < config < overrides), as bs_sim config "
        "prints it. config: a dict, or JSON text. overrides: 'key.path=value' strings.");

    py::class_<RunConfig>(m, "SimConfig", "A resolved run config (as bs_sim run resolves it)")
        .def(py::init([](const py::object& config, const std::vector<std::string>& overrides) {
                 return resolve(config, overrides);
             }),
             py::arg("config"), py::arg("overrides") = std::vector<std::string>{})
        .def_property_readonly("resolved", [](const RunConfig& rc) { return to_python(rc.resolved); })
        .def_readonly("symbol", &RunConfig::symbol)
        .def_readonly("data_file", &RunConfig::bse_path)
        .def_readonly("output_dir", &RunConfig::output_dir)
        .def_readonly("strategy_name", &RunConfig::strategy_name)
        .def_property_readonly("strategy_params", [](const RunConfig& rc) { return to_python(rc.strategy_params); })
        .def("__repr__", [](const RunConfig& rc) {
            return "SimConfig(" + rc.symbol + ", " + rc.strategy_name + ", " + rc.bse_path + ")";
        });

    py::class_<PySimulator>(m, "Simulator",
                            "One run. strategy: a Python object with on_* methods, or None for the config's C++ "
                            "strategy.name. callback_mode: every_event | on_change | timer_only.")
        .def(py::init([](RunConfig rc, py::object strategy, const std::string& mode) {
                 return PySimulator{std::move(rc), std::move(strategy), callback_mode(mode)};
             }),
             py::arg("config"), py::arg("strategy") = py::none(), py::arg("callback_mode") = "every_event")
        .def("run", &PySimulator::run,
             "Runs once and writes the results directory (output.dir). Returns run_meta.json as a dict; with a "
             "Python strategy, python_callbacks counts the calls made.")
        .def_readonly("meta", &PySimulator::meta, "run()'s result (None before the run)")
        .def("tables", &PySimulator::get_tables,
             "After run(): the outputs as numpy structured arrays without a copy (orders, exec_reports, "
             "request_rejects, bbo_samples, positions), plus owners, order_ids and reject_texts, which the "
             "integer owner / order / row columns index. Raw units: ns, 1e-8 fixed, enum codes (result_codes()). "
             "research.sim.results.Results wraps this.");

    m.def("result_codes", &result_codes, "Enum code -> the CSV's string, per enum column of Simulator.tables()");

    m.def(
        "run",
        [](const py::object& config, const std::vector<std::string>& overrides) {
            const RunConfig rc = resolve(config, overrides);
            json extra;
            extra["run"] = {{"command", "blocksprint_sim.run"}, {"overrides", overrides}};
            RunOutcome outcome;
            {
                py::gil_scoped_release release;  // no Python is touched while the simulation runs
                outcome = run_simulation(rc, extra);
            }
            return to_python(outcome.meta);
        },
        py::arg("config"), py::arg("overrides") = std::vector<std::string>{},
        "Runs one simulation as `bs_sim run` does and writes the results directory (output.dir). Returns "
        "run_meta.json as a dict (output_hash, sessions with accounts and stats, counts, inputs).");
}
