#pragma once
// Reconstruction mode (P3.5): replays a .bse file's book batches into an L2Book, with the same rules as the
// Python oracle (research/data/refbook.py):
//
//   SNAP_BEGIN clears the book (invalid until SNAP_END); SNAP_LEVEL sets a level; SNAP_END makes it valid
//   DELTA sets a level's absolute quantity (0 removes it)
//   GAP clears the book and makes it invalid until the next snapshot (a new segment)
//   TRADE doesn't change the book
//
// Checkpoints dump the top N levels per side to CSV (batch,ts_local,valid,side,level,px,qty), so the
// result can be compared with the oracle (research/sim/check_reconstruct.py: validation gate V0).
#include "l2_book.hpp"
#include "replayer.hpp"

#include <cstdint>
#include <ostream>
#include <vector>

namespace sim {

struct ReconstructStats {
    uint64_t records = 0;
    uint64_t batches[4] = {0, 0, 0, 0};  // by BatchKind
    uint64_t book_batches = 0;           // depth events + snapshots + gaps (the oracle's LAST_IN_BATCH rows)
    uint64_t segments = 0;               // segments seen (1 + gaps once data starts)
    uint64_t crossed_batches = 0;        // depth events after which a valid book was crossed or locked
    uint64_t checkpoints = 0;
    int64_t first_ts = 0;
    int64_t last_ts = 0;
    uint64_t ts_backwards = 0;           // batches earlier than the previous one (must be 0)
};

class Reconstructor {
public:
    void apply(const Batch& batch);

    const L2Book& book() const { return book_; }
    bool valid() const { return valid_; }
    const ReconstructStats& stats() const { return stats_; }

private:
    L2Book book_;
    bool valid_ = false;
    bool started_ = false;
    ReconstructStats stats_;
};

struct ReconstructOptions {
    size_t top_n = 20;
    uint64_t every = 0;                 // checkpoint every N book batches (0 = none)
    std::vector<int64_t> at_ts;         // checkpoints: the state after all batches with ts_local <= T
    bool final_checkpoint = true;       // a checkpoint after the last batch
};

// Replays every remaining batch into r; writes checkpoints to dump (if not null). Returns the statistics.
ReconstructStats reconstruct(Replayer& replayer, Reconstructor& r, const ReconstructOptions& options,
                             std::ostream* dump);

// Writes one checkpoint (all top_n levels per side, or one row with valid = 0)
void write_checkpoint(std::ostream& out, uint64_t batch, int64_t ts, const Reconstructor& r, size_t top_n);

// A self-contained piece of a file, e.g. a test fixture (P3.17, bs_sim slice): the book at `from` as one
// synthetic snapshot at `from` (FROM_SNAPSHOT | SYNTHETIC_SNAPSHOT, every level), then every batch with
// from <= ts_local < to, unchanged. Throws std::invalid_argument if the book isn't valid at `from` (before
// the first snapshot, or in a gap). Replays from the start of the file.
std::vector<bse::Record> slice(Replayer& replayer, int64_t from, int64_t to);

} // namespace sim
