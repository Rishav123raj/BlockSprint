#pragma once
// SHA-256 (FIPS 180-4) for run metadata: config, manifest and output hashes (P3.13). Streaming: update()
// any number of times, then hex(). Not for secrets (no constant-time guarantees needed here).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sim {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset() {
        h_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
        used_ = 0;
        bytes_ = 0;
    }

    void update(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes_ += size;
        while (size > 0) {
            const size_t n = std::min(size, buf_.size() - used_);
            std::copy(p, p + n, buf_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += n;
            p += n;
            size -= n;
            if (used_ == buf_.size()) {
                block(buf_.data());
                used_ = 0;
            }
        }
    }
    void update(std::string_view s) { update(s.data(), s.size()); }

    // The digest as 64 lowercase hex digits; the object must be reset() before reuse
    std::string hex() {
        const uint64_t bits = bytes_ * 8;
        const uint8_t one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while (used_ != 56) {
            update(&zero, 1);
        }
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) {
            len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        update(len, 8);
        std::string out;
        char buf[9];
        for (uint32_t v : h_) {
            std::snprintf(buf, sizeof(buf), "%08x", v);
            out += buf;
        }
        return out;
    }

    static std::string of(std::string_view s) {
        Sha256 h;
        h.update(s);
        return h.hex();
    }

    static std::string of_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("sha256: cannot open " + path);
        }
        Sha256 h;
        std::array<char, 1 << 16> chunk;
        while (in) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            h.update(chunk.data(), static_cast<size_t>(in.gcount()));
        }
        return h.hex();
    }

private:
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
                   static_cast<uint32_t>(p[4 * i + 2]) << 8 | static_cast<uint32_t>(p[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buf_{};
    size_t used_ = 0;
    uint64_t bytes_ = 0;
};

} // namespace sim
