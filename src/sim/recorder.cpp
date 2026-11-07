#include "recorder.hpp"

#include "sha256.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace sim {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string px(Price p) { return format_fixed(p); }

// A CSV field that may hold commas or quotes
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

const char* request_type_name(Request::Type t) {
    switch (t) {
        case Request::Type::NEW: return "new";
        case Request::Type::CANCEL: return "cancel";
        case Request::Type::AMEND: return "amend";
        case Request::Type::REPLACE: return "replace";
    }
    return "unknown";
}

const char* event_class_name(size_t c) {
    static const char* names[] = {"market_data", "order_arrive", "strategy_md", "strategy_exec", "timer"};
    return c < 5 ? names[c] : "unknown";
}

json account_json(const Account& a, Price mark) {
    return json{{"position", format_fixed(a.position)},
                {"cash", format_fixed(a.cash)},
                {"fees", format_fixed(a.fees)},
                {"pnl", format_fixed(a.pnl(mark))},
                {"base_volume", format_fixed(a.base_volume)},
                {"quote_volume", format_fixed(a.quote_volume)},
                {"fills", a.fills},
                {"maker_fills", a.maker_fills},
                {"taker_fills", a.taker_fills}};
}

std::string hex(const std::array<uint8_t, 32>& bytes) {
    std::string out;
    char buf[3];
    for (uint8_t b : bytes) {
        std::snprintf(buf, sizeof(buf), "%02x", b);
        out += buf;
    }
    return out;
}

} // namespace

ResultRecorder::ResultRecorder(Simulator& sim, const std::string& dir, uint64_t sample_ns) : sim_(sim), dir_(dir) {
    fs::create_directories(dir_);
    for (Output* o : {&orders_, &reports_, &rejects_, &bbo_, &positions_}) {
        o->out.open(fs::path(dir_) / o->name, std::ios::binary | std::ios::trunc);
        if (!o->out) {
            throw std::runtime_error("recorder: cannot write " + (fs::path(dir_) / o->name).string());
        }
    }
    orders_.out << "sent_ns,arrive_ns,owner,request,order_id,side,order_type,price,quantity,new_price,stp\n";
    reports_.out << "exchange_ns,deliver_ns,owner,seq,order_id,type,reason,side,order_type,price,quantity,"
                    "last_px,last_qty,cum_qty,leaves_qty,liquidity,fee,cum_fee\n";
    rejects_.out << "exchange_ns,deliver_ns,owner,request,order_id,reason,text\n";
    bbo_.out << "ts_ns,owner,data_bid,data_bid_qty,data_ask,data_ask_qty,book_bid,book_bid_qty,book_ask,book_ask_qty,"
                "seen_bid,seen_bid_qty,seen_ask,seen_ask_qty,seen_data_ns\n";
    positions_.out << "ts_ns,owner,position,cash,fees,mark,pnl,fills,maker_fills,taker_fills,open_orders,"
                      "base_volume,quote_volume\n";
    sim_.gateway().set_listener(this);
    if (sample_ns > 0) {
        sim_.set_sampler(sample_ns, [this](uint64_t t) { sample(t); });
    }
}

ResultRecorder::~ResultRecorder() {
    sim_.gateway().set_listener(nullptr);
    if (!finished_) {
        sim_.set_sampler(0, nullptr);
    }
}

void ResultRecorder::on_request(uint32_t session, const Request& r, uint64_t arrive_at) {
    auto& o = row(orders_);
    o << r.sent_at << ',' << arrive_at << ',' << sim_.gateway().owner(session) << ',' << request_type_name(r.type)
      << ',' << r.order_id << ',';
    switch (r.type) {
        case Request::Type::NEW:
        case Request::Type::REPLACE:
            o << sideToString(r.side) << ',' << orderTypeToString(r.order_type) << ',' << px(r.price) << ','
              << px(r.quantity) << ",," << stpPolicyToString(r.stp) << '\n';
            break;
        case Request::Type::AMEND:
            o << ",,," << px(r.quantity) << ',' << (r.new_price ? px(*r.new_price) : std::string()) << ",\n";
            break;
        case Request::Type::CANCEL:
            o << ",,,,,\n";
            break;
    }
}

void ResultRecorder::on_report(uint32_t session, const ExecReport& r, uint64_t deliver_at) {
    auto& o = row(reports_);
    o << r.timestamp << ',' << deliver_at << ',' << sim_.gateway().owner(session) << ',' << r.seq << ','
      << r.order_id << ',' << execTypeToString(r.type) << ','
      << (r.reason != ExecReason::NONE ? execReasonToString(r.reason) : std::string()) << ','
      << sideToString(r.side) << ',' << orderTypeToString(r.order_type) << ',' << px(r.price) << ','
      << px(r.quantity) << ',';
    if (r.type == ExecType::FILL) {
        o << px(r.last_px) << ',' << px(r.last_qty);
    } else {
        o << ',';
    }
    o << ',' << px(r.cum_qty) << ',' << px(r.leaves_qty) << ','
      << (r.type == ExecType::FILL ? liquidityToString(r.liquidity) : std::string()) << ','
      << (r.type == ExecType::FILL ? px(r.fee) : std::string()) << ',' << px(r.cum_fee) << '\n';
}

void ResultRecorder::on_request_reject(uint32_t session, const RequestReject& r, uint64_t deliver_at) {
    row(rejects_) << r.exchange_time << ',' << deliver_at << ',' << sim_.gateway().owner(session) << ','
                  << request_name(r.request) << ',' << r.order_id << ','
                  << (r.reason != ExecReason::NONE ? execReasonToString(r.reason) : std::string()) << ','
                  << quoted(r.text) << '\n';
}

Price ResultRecorder::mark() {
    const Synthesizer& synth = sim_.synthesizer();
    const auto& h = sim_.header();
    std::vector<Level> bid, ask;
    synth.data().top(0, 1, bid);
    synth.data().top(1, 1, ask);
    if (synth.synced() && !bid.empty() && !ask.empty()) {
        last_mark_ = (bid[0].px + ask[0].px) * h.tick_size / 2;
        return last_mark_;
    }
    const Depth d = sim_.engine().depth(h.symbol, 1);
    if (!d.bids.empty() && !d.asks.empty()) {
        last_mark_ = (d.bids[0].price + d.asks[0].price) / 2;
    }
    return last_mark_;
}

void ResultRecorder::sample(uint64_t t) {
    const auto& h = sim_.header();
    const Synthesizer& synth = sim_.synthesizer();
    std::vector<Level> bid, ask;
    if (synth.synced()) {
        synth.data().top(0, 1, bid);
        synth.data().top(1, 1, ask);
    }
    std::string data_cols;
    data_cols += bid.empty() ? "," : px(bid[0].px * h.tick_size) + ',' + px(bid[0].qty * h.lot_size);
    data_cols += ',';
    data_cols += ask.empty() ? "," : px(ask[0].px * h.tick_size) + ',' + px(ask[0].qty * h.lot_size);
    const Depth d = sim_.engine().depth(h.symbol, 1);
    std::string book_cols;
    book_cols += d.bids.empty() ? "," : px(d.bids[0].price) + ',' + px(d.bids[0].quantity);
    book_cols += ',';
    book_cols += d.asks.empty() ? "," : px(d.asks[0].price) + ',' + px(d.asks[0].quantity);

    SimGateway& gw = sim_.gateway();
    for (uint32_t s = 0; s < gw.session_count(); ++s) {
        const ObservedBook& seen = gw.context(s).book();
        const auto sb = seen.valid() ? seen.best_bid() : std::nullopt;
        const auto sa = seen.valid() ? seen.best_ask() : std::nullopt;
        row(bbo_) << t << ',' << gw.owner(s) << ',' << data_cols << ',' << book_cols << ','
                  << (sb ? px(sb->price) + ',' + px(sb->qty) : std::string(",")) << ','
                  << (sa ? px(sa->price) + ',' + px(sa->qty) : std::string(",")) << ','
                  << (seen.batches() ? std::to_string(seen.data_time()) : std::string()) << '\n';
    }
    if (gw.session_count() == 0) {
        row(bbo_) << t << ',' << ',' << data_cols << ',' << book_cols << ",,,,,\n";
    }
    write_positions(t);
}

void ResultRecorder::write_positions(uint64_t t) {
    const Price m = mark();
    SimGateway& gw = sim_.gateway();
    for (uint32_t s = 0; s < gw.session_count(); ++s) {
        const Account& a = gw.exchange_account(s);
        row(positions_) << t << ',' << gw.owner(s) << ',' << px(a.position) << ',' << px(a.cash) << ',' << px(a.fees)
                        << ',' << px(m) << ',' << px(a.pnl(m)) << ',' << a.fills << ',' << a.maker_fills << ','
                        << a.taker_fills << ',' << gw.exchange_open_orders(s) << ',' << px(a.base_volume) << ','
                        << px(a.quote_volume) << '\n';
    }
}

json ResultRecorder::finish(const json& meta) {
    if (finished_) {
        throw std::logic_error("ResultRecorder::finish: already finished");
    }
    finished_ = true;
    write_positions(sim_.now());
    sim_.gateway().set_listener(nullptr);

    json outputs = json::object();
    Sha256 all;
    for (Output* o : {&orders_, &reports_, &rejects_, &bbo_, &positions_}) {
        o->out.close();
        if (!o->out) {
            throw std::runtime_error(std::string("recorder: writing ") + o->name + " failed");
        }
        const std::string sha = Sha256::of_file((fs::path(dir_) / o->name).string());
        outputs[o->name] = json{{"rows", o->rows}, {"sha256", sha}};
        all.update(std::string(o->name) + ' ' + sha + '\n');
    }
    output_hash_ = all.hex();

    const SimStats& st = sim_.stats();
    const auto& h = sim_.header();
    json events = json::object();
    for (size_t c = 0; c < st.events.size(); ++c) {
        events[event_class_name(c)] = st.events[c];
    }
    const Price m = last_mark_;
    json sessions = json::array();
    SimGateway& gw = sim_.gateway();
    for (uint32_t s = 0; s < gw.session_count(); ++s) {
        const SessionStats ss = gw.stats(s);
        sessions.push_back(json{
            {"owner", gw.owner(s)},
            {"strategy", gw.strategy(s).name()},
            {"exchange_account", account_json(gw.exchange_account(s), m)},
            {"strategy_account", account_json(gw.strategy_account(s), m)},
            {"open_orders", gw.exchange_open_orders(s)},
            {"stats",
             {{"orders_sent", ss.orders_sent}, {"cancels_sent", ss.cancels_sent}, {"amends_sent", ss.amends_sent},
              {"replaces_sent", ss.replaces_sent}, {"rate_limited", ss.rate_limited}, {"open_limited", ss.open_limited},
              {"request_rejects", ss.request_rejects}, {"reports", ss.reports}, {"md_batches", ss.md_batches},
              {"timers", ss.timers}, {"held_back", ss.held_back}}},
        });
    }
    const SynthStats& sy = sim_.synthesizer().stats();
    const SimulatorOptions& opt = sim_.options();
    json out{
        {"format", "bs_sim.run/1"},
        {"window",
         {{"start", format_time_ns(st.start_ns)}, {"trading", format_time_ns(st.trading_ns)},
          {"end", format_time_ns(st.end_ns)}, {"start_ns", st.start_ns}, {"trading_ns", st.trading_ns},
          {"end_ns", st.end_ns}}},
        {"simulator",
         {{"seed", opt.seed}, {"queue_model", opt.queue_model},
          {"tie_break", opt.tie_break == TieBreak::DATA_FIRST ? "data_first" : "orders_first"}}},
        {"data_header",
         {{"symbol", h.symbol}, {"tick_size", format_fixed(h.tick_size)}, {"lot_size", format_fixed(h.lot_size)},
          {"records", h.record_count}, {"first_ts_local", h.first_ts_local}, {"last_ts_local", h.last_ts_local},
          {"source_manifest_sha256", hex(h.source_manifest_sha256)}}},
        {"stats",
         {{"fast_forward_batches", st.fast_forward_batches}, {"data_batches", st.data_batches},
          {"data_left", st.data_left != 0}, {"events", events}, {"dropped", st.dropped}, {"samples", st.samples}}},
        {"synthesizer",
         {{"trades", sy.trades}, {"trades_unsynced", sy.trades_unsynced}, {"gaps", sy.gaps},
          {"snapshots", sy.snapshots}, {"crossing_fills", sy.crossing_fills},
          {"tape_strategy_qty", format_fixed(sy.tape_strategy_qty)}}},
        {"final_mark", format_fixed(m)},
        {"sessions", sessions},
        {"outputs", outputs},
        {"output_hash", output_hash_},
    };
    for (const auto& [key, value] : meta.items()) {
        out[key] = value;
    }
    std::ofstream(fs::path(dir_) / "run_meta.json", std::ios::binary) << out.dump(2) << '\n';
    std::ofstream(fs::path(dir_) / "output_hash", std::ios::binary) << output_hash_ << '\n';
    return out;
}

json run_inputs(const RunConfig& rc) {
    json data{{"file", rc.bse_path}, {"symbol", rc.symbol}};
    if (!rc.bse_sha256.empty()) {
        data["sha256"] = rc.bse_sha256;
    }
    if (!rc.manifest_path.empty()) {
        data["manifest"] = rc.manifest_path;
        data["manifest_sha256"] = Sha256::of_file(rc.manifest_path);
    }
    return json{{"config", rc.resolved}, {"config_sha256", Sha256::of(rc.resolved.dump())}, {"data", data}};
}

} // namespace sim
