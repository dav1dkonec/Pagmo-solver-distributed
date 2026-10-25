#include "logger_init.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <filesystem>
#include <vector>

void use_file_logger(const std::string& filePath, const bool enableConsole)
{
    std::vector<spdlog::sink_ptr> sinks;

    // This will ensure that the parent directory always exists (probably not needed as spdlog would create it anyway)
    std::filesystem::create_directories(std::filesystem::path(filePath).parent_path());

    if (enableConsole)
    {
        const auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(console_sink);
    }

    const auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filePath, true);
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [pid:%P] [tid:%t] %v");
    sinks.push_back(file_sink);

    const auto logger = std::make_shared<spdlog::logger>("global_logger", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::warn);

    // set_default_logger() registers the logger itself, so an explicit
    // register_logger() would throw on the second run.
    spdlog::set_default_logger(logger);
}

void use_null_logger()
{
    const auto null_logger = std::make_shared<spdlog::logger>("null_logger",
                                                              std::make_shared<spdlog::sinks::null_sink_mt>());
    null_logger->set_level(spdlog::level::off);
    spdlog::set_default_logger(null_logger);
}

std::string generate_log_filename(const std::string& basePath)
{
    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::format("{:%Y-%m-%d_%H-%M-%S}", now);
    const std::filesystem::path p(basePath);
    const auto stem = p.stem().string();
    const auto ext = p.extension().string();
    const auto dir = p.parent_path();

    return (dir / (stem + "_" + timestamp + ext)).string();
}
