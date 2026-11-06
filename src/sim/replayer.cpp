#include "replayer.hpp"

#include <stdexcept>

namespace sim {

const char* batch_kind_name(BatchKind kind) {
    switch (kind) {
        case BatchKind::DEPTH: return "depth";
        case BatchKind::SNAPSHOT: return "snapshot";
        case BatchKind::TRADE: return "trade";
        case BatchKind::GAP: return "gap";
    }
    return "?";
}

Replayer::Replayer(const std::string& path) : file_(path), header_(bse::parse_header(file_.data(), file_.size())) {
    // The header is 512 bytes, so records start 8-byte aligned in the page-aligned mapping
    records_ = header_.record_count ? reinterpret_cast<const bse::Record*>(file_.data() + bse::kHeaderSize) : nullptr;
}

bool Replayer::next(Batch& batch) {
    const uint64_t n = header_.record_count;
    if (pos_ >= n) {
        return false;
    }
    const uint64_t start = pos_;
    const bse::Record& first = records_[start];
    auto malformed = [&](const std::string& what) {
        return std::runtime_error(file_.path() + ": record " + std::to_string(start) + ": " + what);
    };

    uint64_t end = start + 1;
    switch (first.kind) {
        case bse::TRADE:
            batch.kind = BatchKind::TRADE;
            break;
        case bse::GAP:
            batch.kind = BatchKind::GAP;
            break;
        case bse::DELTA:
            batch.kind = BatchKind::DEPTH;
            end = start;
            while (end < n && records_[end].kind == bse::DELTA && !(records_[end].flags & bse::LAST_IN_BATCH)) {
                ++end;
            }
            if (end >= n || records_[end].kind != bse::DELTA) {
                throw malformed("depth event without a LAST_IN_BATCH record");
            }
            ++end;
            break;
        case bse::SNAP_BEGIN:
            batch.kind = BatchKind::SNAPSHOT;
            while (end < n && records_[end].kind == bse::SNAP_LEVEL) {
                ++end;
            }
            if (end >= n || records_[end].kind != bse::SNAP_END) {
                throw malformed("snapshot without SNAP_END");
            }
            ++end;
            break;
        default:
            throw malformed(std::string("unexpected ") + bse::kind_name(first.kind) + " record at the start of a batch");
    }
    batch.begin = records_ + start;
    batch.end = records_ + end;
    batch.index = start;
    pos_ = end;
    return true;
}

} // namespace sim
