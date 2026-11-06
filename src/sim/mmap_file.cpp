#include "mmap_file.hpp"

#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sim {

#if defined(_WIN32)

MmapFile::MmapFile(const std::string& path) : path_(path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("can't open " + path + " (error " + std::to_string(GetLastError()) + ")");
    }
    file_ = file;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        release();
        throw std::runtime_error("can't get the size of " + path);
    }
    size_ = static_cast<size_t>(size.QuadPart);
    if (size_ == 0) {
        return;  // an empty file can't be mapped; data() stays nullptr
    }
    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        release();
        throw std::runtime_error("can't map " + path + " (error " + std::to_string(GetLastError()) + ")");
    }
    mapping_ = mapping;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        release();
        throw std::runtime_error("can't map a view of " + path + " (error " + std::to_string(GetLastError()) + ")");
    }
    data_ = static_cast<const uint8_t*>(view);
}

void MmapFile::release() noexcept {
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
}

MmapFile::MmapFile(MmapFile&& other) noexcept
    : path_(std::move(other.path_)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      file_(std::exchange(other.file_, nullptr)), mapping_(std::exchange(other.mapping_, nullptr)) {}

MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        file_ = std::exchange(other.file_, nullptr);
        mapping_ = std::exchange(other.mapping_, nullptr);
    }
    return *this;
}

#else

MmapFile::MmapFile(const std::string& path) : path_(path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("can't open " + path);
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("can't get the size of " + path);
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_ > 0) {
        void* view = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) {
            ::close(fd);
            throw std::runtime_error("can't map " + path);
        }
        ::madvise(view, size_, MADV_SEQUENTIAL);
        data_ = static_cast<const uint8_t*>(view);
    }
    ::close(fd);  // the mapping keeps the file referenced
}

void MmapFile::release() noexcept {
    if (data_ != nullptr) {
        ::munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
    }
}

MmapFile::MmapFile(MmapFile&& other) noexcept
    : path_(std::move(other.path_)), data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
    if (this != &other) {
        release();
        path_ = std::move(other.path_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

#endif

MmapFile::~MmapFile() { release(); }

} // namespace sim
