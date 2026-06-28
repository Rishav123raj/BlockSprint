#pragma once
#include <iostream>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>

enum class LogLevel {
    LEVEL_DEBUG,
    LEVEL_INFO,
    LEVEL_WARN,
    LEVEL_ERROR
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void log(LogLevel level, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!log_file_.is_open()) {
            return;
        }
        
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count() % 1000;

        std::tm tm_val;
#if defined(_WIN32) || defined(_WIN64)
        localtime_s(&tm_val, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_val);
#endif

        std::string level_str;
        switch (level) {
            case LogLevel::LEVEL_DEBUG: level_str = "[DEBUG]"; break;
            case LogLevel::LEVEL_INFO: level_str = "[INFO]"; break;
            case LogLevel::LEVEL_WARN: level_str = "[WARN]"; break;
            case LogLevel::LEVEL_ERROR: level_str = "[ERROR]"; break;
        }

        log_file_ << std::put_time(&tm_val, "%Y-%m-%d %H:%M:%S")
                  << "." << std::setw(3) << std::setfill('0') << ms
                  << " " << level_str << " " << message << std::endl;
    }

private:
    Logger() {
        std::filesystem::create_directories("logs");
        log_file_.open("logs/matching_engine.log", std::ios::out | std::ios::app);
    }
    ~Logger() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex mutex_;
    std::ofstream log_file_;
};

#define LOG_DEBUG(msg) Logger::getInstance().log(LogLevel::LEVEL_DEBUG, msg)
#define LOG_INFO(msg) Logger::getInstance().log(LogLevel::LEVEL_INFO, msg)
#define LOG_WARN(msg) Logger::getInstance().log(LogLevel::LEVEL_WARN, msg)
#define LOG_ERROR(msg) Logger::getInstance().log(LogLevel::LEVEL_ERROR, msg)
