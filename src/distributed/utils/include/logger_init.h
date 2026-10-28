#pragma once
#include <filesystem>
#include <string>
#include <spdlog/spdlog.h>

// One per process, created on first use; use_file_logger() and
// use_null_logger() only swap its sinks.
const std::shared_ptr<spdlog::logger>& dist_logger();

void use_file_logger(const std::string& filePath, bool enableConsole = true);
void use_null_logger();

std::string generate_log_filename(const std::string& basePath);
