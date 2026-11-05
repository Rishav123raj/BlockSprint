// Logger: configurable path and level, buffered output, one log per run (P1.15, fixes D19).
#include <doctest.h>
#include "logger.hpp"
#include "matching_engine.hpp"
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// A fresh directory under the system temp directory, removed at the end of the test
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("blocksprint_logger_test_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return (path / name).generic_string(); }
};

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

LoggerOptions options_for(const std::string& path, LogLevel level = LogLevel::LEVEL_DEBUG,
                          LogLevel flush_level = LogLevel::LEVEL_DEBUG) {
    LoggerOptions o;
    o.path = path;
    o.level = level;
    o.flush_level = flush_level;
    return o;
}

const std::regex kLine(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z \[(DEBUG|INFO|WARN|ERROR)\] .*$)");

int g_evaluations = 0;
std::string counted(const std::string& text) {
    ++g_evaluations;
    return text;
}

} // namespace

TEST_SUITE("logger") {

TEST_CASE("level names round-trip") {
    for (LogLevel level : {LogLevel::LEVEL_DEBUG, LogLevel::LEVEL_INFO, LogLevel::LEVEL_WARN, LogLevel::LEVEL_ERROR,
                           LogLevel::LEVEL_OFF}) {
        CHECK(parse_log_level(log_level_name(level)) == level);
    }
    CHECK(parse_log_level("warning") == LogLevel::LEVEL_WARN);
    CHECK_FALSE(parse_log_level("verbose").has_value());
    CHECK_FALSE(parse_log_level("INFO").has_value());
}

TEST_CASE("D19 fixed: the logger is off until opened, so library users write no files") {
    Logger logger;
    CHECK_FALSE(logger.is_open());
    for (LogLevel level : {LogLevel::LEVEL_DEBUG, LogLevel::LEVEL_INFO, LogLevel::LEVEL_WARN, LogLevel::LEVEL_ERROR}) {
        CHECK_FALSE(logger.enabled(level));
    }
    logger.log(LogLevel::LEVEL_ERROR, "dropped");  // no file, no crash

    // The tests never open the global logger: an engine reset (which logs) stays silent
    CHECK_FALSE(Logger::getInstance().is_open());
    MatchingEngine engine;
    engine.reset();
    CHECK_FALSE(Logger::getInstance().is_open());
}

TEST_CASE("lines below the level are dropped; the format is an ISO-8601 UTC timestamp, the level and the message") {
    TempDir dir;
    Logger logger;
    REQUIRE(logger.open(options_for(dir.file("run.log"), LogLevel::LEVEL_WARN)));
    CHECK_FALSE(logger.enabled(LogLevel::LEVEL_INFO));
    CHECK(logger.enabled(LogLevel::LEVEL_WARN));
    logger.log(LogLevel::LEVEL_DEBUG, "d");
    logger.log(LogLevel::LEVEL_INFO, "i");
    logger.log(LogLevel::LEVEL_WARN, "w");
    logger.log(LogLevel::LEVEL_ERROR, "e");
    logger.close();

    auto lines = read_lines(dir.file("run.log"));
    REQUIRE(lines.size() == 2U);
    CHECK(std::regex_match(lines[0], kLine));
    CHECK(lines[0].substr(lines[0].size() - 9) == " [WARN] w");
    CHECK(lines[1].substr(lines[1].size() - 10) == " [ERROR] e");
}

TEST_CASE("set_level changes the threshold of an open logger") {
    TempDir dir;
    Logger logger;
    REQUIRE(logger.open(options_for(dir.file("lvl.log"), LogLevel::LEVEL_ERROR)));
    logger.log(LogLevel::LEVEL_INFO, "before");
    logger.set_level(LogLevel::LEVEL_INFO);
    logger.log(LogLevel::LEVEL_INFO, "after");
    logger.set_level(LogLevel::LEVEL_OFF);
    logger.log(LogLevel::LEVEL_ERROR, "off");
    logger.close();
    auto lines = read_lines(dir.file("lvl.log"));
    REQUIRE(lines.size() == 1U);
    CHECK(lines[0].find("[INFO] after") != std::string::npos);

    logger.set_level(LogLevel::LEVEL_DEBUG);  // closed: stays off
    CHECK_FALSE(logger.enabled(LogLevel::LEVEL_ERROR));
}

TEST_CASE("lines below flush_level stay buffered; a line at flush_level flushes everything") {
    TempDir dir;
    const std::string path = dir.file("buffered.log");
    Logger logger;
    REQUIRE(logger.open(options_for(path, LogLevel::LEVEL_DEBUG, LogLevel::LEVEL_ERROR)));
    logger.log(LogLevel::LEVEL_INFO, "buffered");
    CHECK(read_lines(path).empty());  // no flush per line (1.0 used std::endl)
    logger.log(LogLevel::LEVEL_ERROR, "flushed");
    CHECK(read_lines(path).size() == 2U);
    logger.log(LogLevel::LEVEL_DEBUG, "on close");
    logger.close();
    CHECK(read_lines(path).size() == 3U);
}

TEST_CASE("append keeps earlier runs; append = false truncates") {
    TempDir dir;
    const std::string path = dir.file("same.log");
    Logger logger;
    REQUIRE(logger.open(options_for(path)));
    logger.log(LogLevel::LEVEL_INFO, "one");
    REQUIRE(logger.open(options_for(path)));  // reopening closes the previous file first
    logger.log(LogLevel::LEVEL_INFO, "two");
    logger.close();
    CHECK(read_lines(path).size() == 2U);

    LoggerOptions truncate = options_for(path);
    truncate.append = false;
    REQUIRE(logger.open(truncate));
    logger.log(LogLevel::LEVEL_INFO, "three");
    logger.close();
    auto lines = read_lines(path);
    REQUIRE(lines.size() == 1U);
    CHECK(lines[0].find("three") != std::string::npos);
}

TEST_CASE("open creates missing directories and switches files") {
    TempDir dir;
    Logger logger;
    REQUIRE(logger.open(options_for(dir.file("a/b/first.log"))));
    CHECK(logger.path() == dir.file("a/b/first.log"));
    logger.log(LogLevel::LEVEL_INFO, "first");
    REQUIRE(logger.open(options_for(dir.file("second.log"))));
    logger.log(LogLevel::LEVEL_INFO, "second");
    logger.close();
    CHECK(logger.path().empty());
    CHECK(read_lines(dir.file("a/b/first.log")).size() == 1U);
    CHECK(read_lines(dir.file("second.log")).size() == 1U);
}

TEST_CASE("a path that can't be opened leaves the logger off") {
    TempDir dir;
    Logger logger;
    CHECK_FALSE(logger.open(options_for("")));
    CHECK_FALSE(logger.open(options_for(dir.path.generic_string())));  // a directory, not a file
    CHECK_FALSE(logger.is_open());
    CHECK_FALSE(logger.enabled(LogLevel::LEVEL_ERROR));
}

TEST_CASE("run_log_path gives every run its own file") {
    TempDir dir;
    const std::string base = dir.path.generic_string();
    const std::string first = Logger::run_log_path(base, "matching_engine");
    CHECK(first.rfind(base + "/matching_engine-", 0) == 0);
    CHECK(std::regex_match(first.substr(base.size()), std::regex(R"(/matching_engine-\d{8}-\d{6}\.log)")));
    std::ofstream(first).put('x');
    const std::string second = Logger::run_log_path(base, "matching_engine");
    CHECK(second != first);  // -2 suffix within the same second, a new timestamp otherwise
    std::ofstream(second).put('x');
    CHECK(Logger::run_log_path(base, "matching_engine") != second);
}

TEST_CASE("the macros evaluate the message only when the level is enabled") {
    TempDir dir;
    Logger& global = Logger::getInstance();
    REQUIRE(global.open(options_for(dir.file("global.log"), LogLevel::LEVEL_INFO)));
    g_evaluations = 0;
    LOG_DEBUG(counted("skipped"));
    CHECK(g_evaluations == 0);
    LOG_INFO(counted("written"));
    LOG_ERROR(counted("written too"));
    CHECK(g_evaluations == 2);
    global.close();
    LOG_ERROR(counted("closed"));
    CHECK(g_evaluations == 2);
    CHECK(read_lines(dir.file("global.log")).size() == 2U);
}

TEST_CASE("concurrent writers produce whole lines") {
    TempDir dir;
    Logger logger;
    REQUIRE(logger.open(options_for(dir.file("threads.log"), LogLevel::LEVEL_DEBUG, LogLevel::LEVEL_ERROR)));
    static constexpr int kThreads = 4;
    static constexpr int kLines = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < kLines; ++i) {
                logger.log(LogLevel::LEVEL_INFO, "thread " + std::to_string(t) + " line " + std::to_string(i));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    logger.close();
    auto lines = read_lines(dir.file("threads.log"));
    REQUIRE(lines.size() == static_cast<size_t>(kThreads * kLines));
    int malformed = 0;
    for (const auto& line : lines) {
        malformed += !std::regex_match(line, kLine);
    }
    CHECK(malformed == 0);
}

} // TEST_SUITE("logger")
