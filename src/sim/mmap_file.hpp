#pragma once
// Read-only memory-mapped file: CreateFileMapping / MapViewOfFile on Windows, mmap on POSIX (P3.4).
// The replayer reads .bse records straight from the mapping, so a day of data costs no heap memory and
// the OS pages it in as the replay advances.
#include <cstddef>
#include <cstdint>
#include <string>

namespace sim {

class MmapFile {
public:
    // Maps the whole file; throws std::runtime_error if it can't be opened or mapped
    explicit MmapFile(const std::string& path);
    ~MmapFile();

    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;
    MmapFile(MmapFile&& other) noexcept;
    MmapFile& operator=(MmapFile&& other) noexcept;

    const uint8_t* data() const { return data_; }  // nullptr for an empty file
    size_t size() const { return size_; }
    const std::string& path() const { return path_; }

private:
    void release() noexcept;

    std::string path_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#if defined(_WIN32)
    void* file_ = nullptr;     // HANDLE
    void* mapping_ = nullptr;  // HANDLE
#endif
};

} // namespace sim
