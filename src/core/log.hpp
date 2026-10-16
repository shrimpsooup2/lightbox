#pragma once

#include <filesystem>
#include <format>
#include <string_view>
#include <utility>

// Minimal thread-safe logger. Writes to stderr and, once configured, to a log file.
// Usage: lb::log::info("Imported {} meshes", count);
namespace lb::log {

enum class Level { Debug, Info, Warn, Error };

void setMinLevel(Level level);

// Opens (truncates) the log file. The previous file at that path is kept as "<stem>.prev<ext>".
// Returns false if the file cannot be opened; stderr logging continues either way.
bool setFile(const std::filesystem::path& path);

void write(Level level, std::string_view message);

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace lb::log
