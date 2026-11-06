#include "bse.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace sim::bse {

namespace {

// Little-endian load/store (all supported targets are little-endian; checked in parse_header)
template <typename T>
T load(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
void store(uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

bool host_is_little_endian() {
    const uint16_t probe = 1;
    uint8_t first;
    std::memcpy(&first, &probe, 1);
    return first == 1;
}

} // namespace

const char* kind_name(uint8_t kind) {
    switch (kind) {
        case SNAP_BEGIN: return "SNAP_BEGIN";
        case SNAP_LEVEL: return "SNAP_LEVEL";
        case SNAP_END: return "SNAP_END";
        case DELTA: return "DELTA";
        case TRADE: return "TRADE";
        case GAP: return "GAP";
        default: return "?";
    }
}

Header parse_header(const uint8_t* data, size_t file_size) {
    if (!host_is_little_endian()) {
        throw std::runtime_error(".bse files are little-endian; big-endian hosts aren't supported");
    }
    if (data == nullptr || file_size < kHeaderSize) {
        throw std::runtime_error("not a .bse file: shorter than the 512-byte header");
    }
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error("not a .bse file: wrong magic");
    }
    Header h;
    h.version = load<uint32_t>(data + 8);
    h.record_size = load<uint32_t>(data + 12);
    if (h.version != kVersion || h.record_size != kRecordSize) {
        throw std::runtime_error("unsupported .bse format version " + std::to_string(h.version) + " / record size " +
                                 std::to_string(h.record_size));
    }
    h.record_count = load<uint64_t>(data + 16);
    h.first_ts_local = load<int64_t>(data + 24);
    h.last_ts_local = load<int64_t>(data + 32);
    std::memcpy(h.source_manifest_sha256.data(), data + 40, 32);
    const char* sym = reinterpret_cast<const char*>(data + 72);
    h.symbol.assign(sym, strnlen(sym, 32));
    h.tick_size = load<int64_t>(data + 104);
    h.lot_size = load<int64_t>(data + 112);
    h.price_decimals = data[120];
    h.qty_decimals = data[121];
    h.min_notional = load<int64_t>(data + 122);
    for (size_t i = 130; i < kHeaderSize; ++i) {
        if (data[i] != 0) {
            throw std::runtime_error(".bse header: reserved bytes are not zero");
        }
    }
    const uint64_t expected = kHeaderSize + h.record_count * kRecordSize;
    if (file_size != expected) {
        throw std::runtime_error(".bse file is " + std::to_string(file_size) + " bytes; " +
                                 std::to_string(h.record_count) + " records need " + std::to_string(expected));
    }
    if (h.tick_size <= 0 || h.lot_size <= 0) {
        throw std::runtime_error(".bse header: tick and lot sizes must be positive");
    }
    return h;
}

std::array<uint8_t, kHeaderSize> encode_header(const Header& h) {
    std::array<uint8_t, kHeaderSize> out{};
    std::memcpy(out.data(), kMagic, sizeof(kMagic));
    store<uint32_t>(out.data() + 8, h.version);
    store<uint32_t>(out.data() + 12, h.record_size);
    store<uint64_t>(out.data() + 16, h.record_count);
    store<int64_t>(out.data() + 24, h.first_ts_local);
    store<int64_t>(out.data() + 32, h.last_ts_local);
    std::memcpy(out.data() + 40, h.source_manifest_sha256.data(), 32);
    std::memcpy(out.data() + 72, h.symbol.data(), std::min<size_t>(h.symbol.size(), 32));
    store<int64_t>(out.data() + 104, h.tick_size);
    store<int64_t>(out.data() + 112, h.lot_size);
    out[120] = h.price_decimals;
    out[121] = h.qty_decimals;
    store<int64_t>(out.data() + 122, h.min_notional);
    return out;
}

void write_file(const std::string& path, Header header, const std::vector<Record>& records) {
    header.record_count = records.size();
    header.first_ts_local = records.empty() ? 0 : records.front().ts_local;
    header.last_ts_local = records.empty() ? 0 : records.back().ts_local;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("can't write " + path);
    }
    auto head = encode_header(header);
    out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
    if (!records.empty()) {
        out.write(reinterpret_cast<const char*>(records.data()),
                  static_cast<std::streamsize>(records.size() * sizeof(Record)));
    }
    if (!out) {
        throw std::runtime_error("error writing " + path);
    }
}

} // namespace sim::bse
