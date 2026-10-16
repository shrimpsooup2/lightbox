#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>

namespace lb::log {
namespace {

struct State {
    std::mutex mutex;
    Level minLevel = Level::Debug;
    std::ofstream file;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
    static State s;
    return s;
}

constexpr std::string_view levelName(Level level) {
    switch (level) {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info ";
    case Level::Warn:
        return "warn ";
    case Level::Error:
        return "error";
    }
    return "?    ";
}

} // namespace

void setMinLevel(Level level) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.minLevel = level;
}

bool setFile(const std::filesystem::path& path) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (std::filesystem::exists(path, ec)) {
        auto previous = path;
        previous.replace_filename(path.stem().string() + ".prev" + path.extension().string());
        std::filesystem::rename(path, previous, ec);
    }
    s.file.close();
    s.file.open(path, std::ios::out | std::ios::trunc);
    return s.file.is_open();
}

void write(Level level, std::string_view message) {
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (level < s.minLevel) {
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    const std::string line = std::format("[{:9.3f}] [{}] {}\n", seconds, levelName(level), message);
    std::fputs(line.c_str(), stderr);
    if (s.file.is_open()) {
        s.file << line;
        s.file.flush();
    }
}

} // namespace lb::log
