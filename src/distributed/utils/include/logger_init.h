#pragma once
#include <filesystem>
#include <string>
#include <spdlog/spdlog.h>

// The logger this library writes to. Separate from spdlog's default logger, so that
// configuring logging here does not silence the host application. The object itself lives
// for the whole process; use_file_logger() and use_null_logger() only swap its sinks.
const std::shared_ptr<spdlog::logger>& dist_logger();

void use_file_logger(const std::string& filePath, bool enableConsole = true);
void use_null_logger();

std::string generate_log_filename(const std::string& basePath);
