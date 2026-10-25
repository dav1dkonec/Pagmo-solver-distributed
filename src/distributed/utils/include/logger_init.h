#pragma once
#include <filesystem>
#include <string>
#include <spdlog/spdlog.h>

void use_file_logger(const std::string& filePath, bool enableConsole = true);
void use_null_logger();

std::string generate_log_filename(const std::string& basePath);
