#include "logger_control.h"

logger_control::logger_control()
{
    use_null_logger();
}

logger_control::~logger_control()
{
    if (_loggerEnabled)
    {
        // Swap in a logger that discards everything. spdlog::shutdown() would instead
        // leave the default logger pointer null and crash the next log call.
        use_null_logger();
    }
}

void logger_control::enable_logging(const std::string& logFilePath, bool writeToConsole)
{
    _loggerEnabled = true;
    use_file_logger(logFilePath, writeToConsole);
}

void logger_control::enable_logging(bool writeToConsole)
{
    _loggerEnabled = true;
    const std::string fileName = generate_log_filename(get_default_log_name()) + ".log";
    use_file_logger(fileName, writeToConsole);
}

void logger_control::disable_logging()
{
    _loggerEnabled = false;
    use_null_logger();
}

bool logger_control::is_logging_enabled() const
{
    return _loggerEnabled;
}
