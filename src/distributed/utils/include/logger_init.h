#pragma once
#include <filesystem>
#include <string>
#include <spdlog/spdlog.h>

// One per linked image, created on first use; use_file_logger() and use_null_logger() only swap
// its sinks. A dynamically loaded module holds its own instance, hence dist_log_sink below.
const std::shared_ptr<spdlog::logger>& dist_logger();

void use_file_logger(const std::string& filePath, bool enableConsole = true);
void use_null_logger();

// Level values are spdlog::level::level_enum; only ints and a string cross the module boundary.
using dist_log_sink = void (*)(int level, const char* message);

// Writes into this image's logger. Its address is what the host hands to a loaded module.
extern "C" void dist_log_write(int level, const char* message);

std::string generate_log_filename(const std::string& basePath);
