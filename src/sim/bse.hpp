#pragma once
// The .bse replay file format, v1 (written by research/data/pack.py; plan section 14.2).
//
//   header, 512 bytes (little-endian)
//     0 magic "BSEVENT\0" | 8 version u32 | 12 record size u32 | 16 record count u64
//     24 first ts_local i64 | 32 last ts_local i64 | 40 SHA-256 of the source manifest (32 bytes)
//     72 symbol (32 bytes, zero-padded) | 104 tick size i64 | 112 lot size i64 (1e-8 units)
//     120 price decimals u8 | 121 qty decimals u8 | 122 min notional i64 (1e-8 units) | 130.. zero
//   record, 40 bytes
//     0 ts_local i64 | 8 ts_exch i64 | 16 px i64 (ticks) | 24 qty i64 (lots) | 32 seq u32 (low 32 bits)
//     36 kind u8 | 37 side u8 | 38 flags u16
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sim::bse {

constexpr char kMagic[8] = {'B', 'S', 'E', 'V', 'E', 'N', 'T', '\0'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRecordSize = 40;
constexpr size_t kHeaderSize = 512;

// Record kinds (research/data/schema.py)
enum Kind : uint8_t {
    SNAP_BEGIN = 0,
    SNAP_LEVEL = 1,
    SNAP_END = 2,
    DELTA = 3,
    TRADE = 4,
    GAP = 5,
};

enum Side : uint8_t {
    BID = 0,  // book side; for trades: aggressor buy
    ASK = 1,  // book side; for trades: aggressor sell
};

enum Flag : uint16_t {
    LAST_IN_BATCH = 1 << 0,
    FROM_SNAPSHOT = 1 << 1,
    DUPLICATE_SUSPECT = 1 << 2,
    TS_CLAMPED = 1 << 3,
    SYNTHETIC_SNAPSHOT = 1 << 4,
    OFF_GRID = 1 << 5,
};

struct Record {
    int64_t ts_local;
    int64_t ts_exch;
    int64_t px;   // ticks
    int64_t qty;  // lots
    uint32_t seq; // low 32 bits of the update ID / trade ID
    uint8_t kind;
    uint8_t side;
    uint16_t flags;
};
static_assert(sizeof(Record) == kRecordSize, "Record must match the 40-byte file layout");
static_assert(offsetof(Record, seq) == 32 && offsetof(Record, kind) == 36 && offsetof(Record, flags) == 38,
              "Record field offsets must match the file layout");

struct Header {
    uint32_t version = kVersion;
    uint32_t record_size = kRecordSize;
    uint64_t record_count = 0;
    int64_t first_ts_local = 0;
    int64_t last_ts_local = 0;
    std::array<uint8_t, 32> source_manifest_sha256{};
    std::string symbol;
    int64_t tick_size = 0;  // 1e-8 units
    int64_t lot_size = 0;   // 1e-8 units
    uint8_t price_decimals = 0;
    uint8_t qty_decimals = 0;
    int64_t min_notional = 0;  // 1e-8 units
};

const char* kind_name(uint8_t kind);

// Parses and validates a header from the start of a file of file_size bytes. Throws std::runtime_error
// on a wrong magic, an unsupported version or record size, non-zero reserved bytes, or a size that
// doesn't match the record count.
Header parse_header(const uint8_t* data, size_t file_size);

// Serializes a header (record_count and the timestamps as given)
std::array<uint8_t, kHeaderSize> encode_header(const Header& h);

// Writes a complete file (for tests and fixtures). Sets the header's count and first/last ts_local
// from the records.
void write_file(const std::string& path, Header header, const std::vector<Record>& records);

} // namespace sim::bse
