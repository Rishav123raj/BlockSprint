#include "reconstruct.hpp"

#include <algorithm>
#include <stdexcept>

namespace sim {

void Reconstructor::apply(const Batch& batch) {
    stats_.records += batch.size();
    stats_.batches[static_cast<int>(batch.kind)] += 1;
    const int64_t ts = batch.ts_local();
    if (!started_) {
        stats_.first_ts = ts;
        stats_.segments = 1;
        started_ = true;
    } else if (ts < stats_.last_ts) {
        ++stats_.ts_backwards;
    }
    stats_.last_ts = ts;

    switch (batch.kind) {
        case BatchKind::TRADE:
            return;
        case BatchKind::GAP:
            book_.clear();
            valid_ = false;
            ++stats_.segments;
            break;
        case BatchKind::SNAPSHOT:
            book_.clear();
            for (const bse::Record* r = batch.begin + 1; r + 1 < batch.end; ++r) {
                book_.set(r->side, r->px, r->qty);
            }
            valid_ = true;
            break;
        case BatchKind::DEPTH:
            for (const bse::Record* r = batch.begin; r < batch.end; ++r) {
                book_.set(r->side, r->px, r->qty);
            }
            if (valid_ && book_.crossed()) {
                ++stats_.crossed_batches;
            }
            break;
    }
    ++stats_.book_batches;
}

void write_checkpoint(std::ostream& out, uint64_t batch, int64_t ts, const Reconstructor& r, size_t top_n) {
    if (!r.valid()) {
        out << batch << ',' << ts << ",0,,,,\n";
        return;
    }
    std::vector<Level> levels;
    for (uint8_t side = 0; side < 2; ++side) {
        r.book().top(side, top_n, levels);
        for (size_t i = 0; i < levels.size(); ++i) {
            out << batch << ',' << ts << ",1," << int(side) << ',' << i << ',' << levels[i].px << ','
                << levels[i].qty << '\n';
        }
    }
}

ReconstructStats reconstruct(Replayer& replayer, Reconstructor& r, const ReconstructOptions& options,
                             std::ostream* dump) {
    std::vector<int64_t> at = options.at_ts;
    std::sort(at.begin(), at.end());
    size_t next_at = 0;
    uint64_t checkpoints = 0;
    if (dump) {
        *dump << "batch,ts_local,valid,side,level,px,qty\n";
    }

    Batch batch{};
    int64_t last_ts = 0;
    while (replayer.next(batch)) {
        // Time checkpoints: the state after every batch with ts_local <= T
        while (next_at < at.size() && batch.ts_local() > at[next_at]) {
            if (dump) {
                write_checkpoint(*dump, r.stats().book_batches, at[next_at], r, options.top_n);
            }
            ++checkpoints;
            ++next_at;
        }
        r.apply(batch);
        last_ts = batch.ts_local();
        if (options.every && batch.is_book() && r.stats().book_batches % options.every == 0) {
            if (dump) {
                write_checkpoint(*dump, r.stats().book_batches, last_ts, r, options.top_n);
            }
            ++checkpoints;
        }
    }
    for (; next_at < at.size(); ++next_at) {  // times after the last batch
        if (dump) {
            write_checkpoint(*dump, r.stats().book_batches, at[next_at], r, options.top_n);
        }
        ++checkpoints;
    }
    if (options.final_checkpoint) {
        if (dump) {
            write_checkpoint(*dump, r.stats().book_batches, last_ts, r, options.top_n);
        }
        ++checkpoints;
    }
    ReconstructStats stats = r.stats();
    stats.checkpoints = checkpoints;
    return stats;
}

std::vector<bse::Record> slice(Replayer& replayer, int64_t from, int64_t to) {
    if (to <= from) {
        throw std::invalid_argument("slice: the end must be after the start");
    }
    replayer.rewind();
    Reconstructor r;
    std::vector<bse::Record> out;
    std::vector<Level> levels;
    bool started = false;
    Batch b;
    while (replayer.next(b) && b.ts_local() < to) {
        if (b.ts_local() < from) {
            r.apply(b);
            continue;
        }
        if (!started && !r.valid() && b.kind == BatchKind::SNAPSHOT) {
            started = true;  // the data's own snapshot establishes the book
        }
        if (!started) {
            if (!r.valid()) {
                throw std::invalid_argument("slice: the book isn't valid at the start (before the first snapshot, "
                                            "or in a gap)");
            }
            const uint16_t f = bse::FROM_SNAPSHOT | bse::SYNTHETIC_SNAPSHOT;
            out.push_back(bse::Record{from, 0, 0, 0, 0, bse::SNAP_BEGIN, 0, f});
            for (uint8_t side = 0; side < 2; ++side) {
                r.book().top(side, r.book().levels(side), levels);
                for (const Level& l : levels) {
                    out.push_back(bse::Record{from, 0, l.px, l.qty, 0, bse::SNAP_LEVEL, side, f});
                }
            }
            out.push_back(bse::Record{from, 0, 0, 0, 0, bse::SNAP_END, 0,
                                      static_cast<uint16_t>(f | bse::LAST_IN_BATCH)});
            started = true;
        }
        out.insert(out.end(), b.begin, b.end);
    }
    if (!started) {
        throw std::invalid_argument("slice: no data in the window");
    }
    return out;
}

std::vector<BboPoint> bbo_timeline(const std::string& path, uint64_t from_ns, uint64_t to_ns) {
    Replayer replayer(path);
    const int64_t tick = replayer.header().tick_size, lot = replayer.header().lot_size;
    Reconstructor r;
    std::vector<BboPoint> out;
    BboPoint cur{from_ns, 0, 0, 0, 0};
    auto current = [&](uint64_t t) {
        BboPoint p{t, 0, 0, 0, 0};
        if (r.valid()) {
            if (const auto b = r.book().best_bid()) {
                p.bid = b->px * tick;
                p.bid_qty = b->qty * lot;
            }
            if (const auto a = r.book().best_ask()) {
                p.ask = a->px * tick;
                p.ask_qty = a->qty * lot;
            }
        }
        return p;
    };
    auto same = [](const BboPoint& a, const BboPoint& b) {
        return a.bid == b.bid && a.bid_qty == b.bid_qty && a.ask == b.ask && a.ask_qty == b.ask_qty;
    };

    uint64_t data_time = 0;
    bool started = false;
    Batch b{};
    while (replayer.next(b)) {
        const uint64_t ts = b.ts_local() > 0 ? static_cast<uint64_t>(b.ts_local()) : 0;
        data_time = std::max(data_time, ts);
        if (to_ns && data_time >= to_ns) {
            break;
        }
        if (!started && data_time >= from_ns) {
            out.push_back(cur);  // the book as of from_ns
            started = true;
        }
        r.apply(b);
        if (!b.is_book()) {
            continue;
        }
        const BboPoint p = current(started ? data_time : from_ns);
        if (!started) {
            cur = p;
        } else if (!same(p, out.back())) {
            out.push_back(p);
        }
    }
    if (!started) {
        out.push_back(cur);
    }
    return out;
}

} // namespace sim
