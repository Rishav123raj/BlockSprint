#pragma once
// Replayer (P3.4): opens a .bse file, validates it, and hands out its records as batches, the units in
// which the market changes:
//
//   DEPTH     consecutive DELTA records up to the one flagged LAST_IN_BATCH (one depth event)
//   SNAPSHOT  SNAP_BEGIN, SNAP_LEVEL..., SNAP_END
//   TRADE     one TRADE record
//   GAP       one GAP record (updates were lost; a new segment starts)
//
// Records are read straight from the memory mapping; a batch is a [begin, end) range over them.
#include "bse.hpp"
#include "mmap_file.hpp"

#include <cstdint>
#include <string>

namespace sim {

enum class BatchKind : uint8_t { DEPTH, SNAPSHOT, TRADE, GAP };

const char* batch_kind_name(BatchKind kind);

struct Batch {
    BatchKind kind;
    const bse::Record* begin;
    const bse::Record* end;
    uint64_t index;  // position of the first record in the file
    int64_t shift = 0;  // P6.6: ticks the market had moved by permanent impact when the batch was applied
    uint32_t symbol = 0;  // P6.7: index of the batch's file in the run's symbols

    int64_t ts_local() const { return begin->ts_local; }
    size_t size() const { return static_cast<size_t>(end - begin); }
    bool is_book() const { return kind != BatchKind::TRADE; }
};

class Replayer {
public:
    explicit Replayer(const std::string& path);

    const bse::Header& header() const { return header_; }
    const bse::Record* records() const { return records_; }
    uint64_t record_count() const { return header_.record_count; }

    // The next batch, or false at the end. Throws std::runtime_error on a malformed batch (a depth event
    // or snapshot that is cut off, or a record of an unknown kind), naming the record index.
    bool next(Batch& batch);

    // Position of the next record to read
    uint64_t position() const { return pos_; }
    void rewind() { pos_ = 0; }

private:
    MmapFile file_;
    bse::Header header_;
    const bse::Record* records_ = nullptr;
    uint64_t pos_ = 0;
};

} // namespace sim
