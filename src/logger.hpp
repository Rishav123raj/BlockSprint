#pragma once
// Diagnostics logger (2.0, P1.15, fixes D19).
//
// The logger is off until open() is called: tests, benchmarks and backtests that never open it write
// nothing and create no files. The macros check the level before building the message, so a disabled
// LOG_DEBUG costs one atomic load. Lines end with '\n'; only lines at or above flush_level are flushed
// at once, the rest stay buffered until close() or a later flush.
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

enum class LogLevel {
    LEVEL_DEBUG,
    LEVEL_INFO,
    LEVEL_WARN,
    LEVEL_ERROR,
    LEVEL_OFF  // threshold only: nothing is logged
};

inline const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::LEVEL_DEBUG: return "debug";
        case LogLevel::LEVEL_INFO: return "info";
        case LogLevel::LEVEL_WARN: return "warn";
        case LogLevel::LEVEL_ERROR: return "error";
        case LogLevel::LEVEL_OFF: return "off";
    }
    return "?";
}

// "debug", "info", "warn" (or "warning"), "error", "off"; nullopt for anything else
inline std::optional<LogLevel> parse_log_level(std::string_view name) {
    if (name == "debug") return LogLevel::LEVEL_DEBUG;
    if (name == "info") return LogLevel::LEVEL_INFO;
    if (name == "warn" || name == "warning") return LogLevel::LEVEL_WARN;
    if (name == "error") return LogLevel::LEVEL_ERROR;
    if (name == "off") return LogLevel::LEVEL_OFF;
    return std::nullopt;
}

struct LoggerOptions {
    std::string path;                             // log file; parent directories are created
    LogLevel level = LogLevel::LEVEL_INFO;        // lines below this level are dropped
    LogLevel flush_level = LogLevel::LEVEL_WARN;  // lines at or above this level are flushed at once
    bool append = true;                           // false truncates an existing file
};

class Logger {
public:
    Logger() = default;
    ~Logger() { close(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // The process-wide logger the LOG_* macros write to
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    // Opens (or switches to) a log file. Returns false, leaving the logger off, if the file can't be opened.
    bool open(const LoggerOptions& options) {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
        std::error_code ec;
        std::filesystem::path p(options.path);
        if (p.empty()) {
            return false;
        }
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
        file_.open(p, std::ios::out | (options.append ? std::ios::app : std::ios::trunc));
        if (!file_.is_open()) {
            return false;
        }
        path_ = options.path;
        flush_level_ = options.flush_level;
        threshold_.store(static_cast<int>(options.level), std::memory_order_relaxed);
        return true;
    }

    // Flushes and closes the file; the logger is off afterwards
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return file_.is_open();
    }

    std::string path() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return path_;
    }

    // True if a line at this level would be written (false whenever the logger is off)
    bool enabled(LogLevel level) const {
        return level != LogLevel::LEVEL_OFF &&
               static_cast<int>(level) >= threshold_.load(std::memory_order_relaxed);
    }

    // Changes the threshold of an open logger; has no effect while it is off
    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            threshold_.store(static_cast<int>(level), std::memory_order_relaxed);
        }
    }

    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open() || !enabled(level)) {
            return;
        }
        file_ << timestamp_utc() << " [" << level_tag(level) << "] " << message << '\n';
        if (level >= flush_level_) {
            file_.flush();
        }
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.flush();
        }
    }

    // A fresh file name for one run: <dir>/<prefix>-YYYYMMDD-HHMMSS.log (UTC), with -2, -3, ...
    // appended if a run in the same second already used it
    static std::string run_log_path(const std::string& dir, const std::string& prefix) {
        std::time_t now = std::time(nullptr);
        std::tm tm_val = utc(now);
        std::ostringstream stem;
        stem << prefix << '-' << std::put_time(&tm_val, "%Y%m%d-%H%M%S");
        std::filesystem::path base(dir);
        std::filesystem::path candidate = base / (stem.str() + ".log");
        std::error_code ec;
        for (int n = 2; std::filesystem::exists(candidate, ec); ++n) {
            candidate = base / (stem.str() + "-" + std::to_string(n) + ".log");
        }
        return candidate.generic_string();
    }

private:
    void close_locked() {
        threshold_.store(static_cast<int>(LogLevel::LEVEL_OFF), std::memory_order_relaxed);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        path_.clear();
    }

    static const char* level_tag(LogLevel level) {
        switch (level) {
            case LogLevel::LEVEL_DEBUG: return "DEBUG";
            case LogLevel::LEVEL_INFO: return "INFO";
            case LogLevel::LEVEL_WARN: return "WARN";
            case LogLevel::LEVEL_ERROR: return "ERROR";
            case LogLevel::LEVEL_OFF: break;
        }
        return "?";
    }

    static std::tm utc(std::time_t t) {
        std::tm tm_val{};
#if defined(_WIN32) || defined(_WIN64)
        gmtime_s(&tm_val, &t);
#else
        gmtime_r(&t, &tm_val);
#endif
        return tm_val;
    }

    // 2026-11-05T17:23:16.961Z
    static std::string timestamp_utc() {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm tm_val = utc(std::chrono::system_clock::to_time_t(now));
        std::ostringstream out;
        out << std::put_time(&tm_val, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
        return out.str();
    }

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::string path_;
    LogLevel flush_level_ = LogLevel::LEVEL_WARN;
    std::atomic<int> threshold_{static_cast<int>(LogLevel::LEVEL_OFF)};
};

// The message expression is evaluated only if the level is enabled
#define BLOCKSPRINT_LOG(level, msg)                          \
    do {                                                     \
        Logger& blocksprint_logger_ = Logger::getInstance(); \
        if (blocksprint_logger_.enabled(level)) {            \
            blocksprint_logger_.log(level, msg);             \
        }                                                    \
    } while (0)

#define LOG_DEBUG(msg) BLOCKSPRINT_LOG(LogLevel::LEVEL_DEBUG, msg)
#define LOG_INFO(msg) BLOCKSPRINT_LOG(LogLevel::LEVEL_INFO, msg)
#define LOG_WARN(msg) BLOCKSPRINT_LOG(LogLevel::LEVEL_WARN, msg)
#define LOG_ERROR(msg) BLOCKSPRINT_LOG(LogLevel::LEVEL_ERROR, msg)
